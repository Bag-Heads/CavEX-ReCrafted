#include "item_nbt.h"
#include "../cNBT/nbt.h"
#include "inventory.h"
#include <stdlib.h>
#include <limits.h>
#include <string.h>

struct item_record {
	uint16_t length;
	uint8_t offset[4]; /* id, Damage, Count, Slot; 255 means absent */
	uint8_t bytes[ITEM_NBT_RECORD_BYTES];
};
static struct item_record records[ITEM_NBT_RECORDS];
static unsigned record_count;
static unsigned record_exhaustions;
unsigned item_nbt_exhaustions(void) {
	return __atomic_load_n(&record_exhaustions, __ATOMIC_RELAXED);
}
unsigned item_nbt_templates(void) { return record_count; }

static nbt_node* materialize(const nbt_node* n) {
	if(!n || n->type!=TAG_RAW_COMPOUND || n->payload.tag_byte_array.length<0) return NULL;
	size_t length=(size_t)n->payload.tag_byte_array.length;
	if(length>SIZE_MAX-3) return NULL;
	uint8_t* bytes=malloc(length+3);
	if(!bytes) return NULL;
	bytes[0]=TAG_COMPOUND; bytes[1]=bytes[2]=0;
	memcpy(bytes+3,n->payload.tag_byte_array.data,length);
	nbt_node* parsed=nbt_parse(bytes,length+3); free(bytes); return parsed;
}

static nbt_node* child(const nbt_node* n, const char* name) {
	if(!n || n->type != TAG_COMPOUND || !n->payload.tag_compound) return NULL;
	struct list_head* p;
	list_for_each(p, &n->payload.tag_compound->entry) {
		nbt_node* c = list_entry(p, struct nbt_list, entry)->data;
		if(c && c->name && !strcmp(c->name, name)) return c;
	}
	return NULL;
}

nbt_node* item_nbt_child(const nbt_node* n, const char* name) {
	return child(n,name);
}

bool item_nbt_equal(const nbt_node* a, const nbt_node* b) {
	struct buffer x=nbt_dump_binary(a), y=nbt_dump_binary(b);
	bool same=x.data && y.data && x.len==y.len && !memcmp(x.data,y.data,x.len);
	buffer_free(&x); buffer_free(&y); return same;
}

bool item_nbt_replace(nbt_node* parent, const char* name, nbt_node* replacement) {
	if(!replacement || !parent || parent->type!=TAG_COMPOUND) {
		nbt_free(replacement); return false;
	}
	char* copy=malloc(strlen(name)+1);
	if(!copy) { nbt_free(replacement); return false; }
	memcpy(copy,name,strlen(name)+1); free(replacement->name); replacement->name=copy;
	struct list_head* p;
	list_for_each(p,&parent->payload.tag_compound->entry) {
		struct nbt_list* w=list_entry(p,struct nbt_list,entry);
		if(w->data->name && !strcmp(w->data->name,name)) {
			nbt_free(w->data); w->data=replacement; return true;
		}
	}
	struct nbt_list* w=malloc(sizeof(*w));
	if(!w) { nbt_free(replacement); return false; }
	w->data=replacement; list_add_tail(&w->entry,&parent->payload.tag_compound->entry); return true;
}

static bool scalar(const nbt_node* n, struct item_data* it) {
	if(n && n->type==TAG_RAW_COMPOUND) {
		nbt_node* parsed=materialize(n);
		bool ok=scalar(parsed,it); nbt_free(parsed); return ok;
	}
	nbt_node* id = child(n, "id"), *d = child(n, "Damage"), *c = child(n, "Count");
	if(!id || id->type != TAG_SHORT || !d || d->type != TAG_SHORT
	   || !c || c->type != TAG_BYTE) return false;
	*it = (struct item_data){.id = (uint16_t)id->payload.tag_short,
		.durability = (uint16_t)d->payload.tag_short,
		.count = (uint8_t)c->payload.tag_byte};
	return true;
}

static struct buffer payload(const nbt_node* n) {
	struct buffer b = BUFFER_INIT;
	if(n && n->type==TAG_RAW_COMPOUND) {
		if(n->payload.tag_byte_array.length>0)
			(void)buffer_append(&b,n->payload.tag_byte_array.data,(size_t)n->payload.tag_byte_array.length);
		return b;
	}
	if(!n || n->type != TAG_COMPOUND || !n->payload.tag_compound) return b;
	nbt_node* raw = n->payload.tag_compound->data;
	if(raw && raw->type == TAG_BYTE_ARRAY) {
		if(raw->payload.tag_byte_array.length > 0
		   && !buffer_append(&b, raw->payload.tag_byte_array.data,
						 (size_t)raw->payload.tag_byte_array.length)) return b;
		buffer_free(&b);
		return BUFFER_INIT;
	}
	nbt_node copy = *n;
	copy.name = NULL;
	b = nbt_dump_binary(&copy);
	if(b.data && b.len) {
		memmove(b.data, (uint8_t*)b.data + 1, --b.len);
	}
	return b;
}

/* Bounded raw scanner; offsets refer to original bytes, not a re-encoded tree.
 * Unknown tag types cannot be skipped safely and pin the complete record. */
static bool skip(const uint8_t* b, size_t n, size_t* at, unsigned type, unsigned depth) {
	if(depth > 32 || *at > n) return false;
	size_t len = 0;
	if(type >= 1 && type <= 6) {
		static const uint8_t widths[] = {0,1,2,4,8,4,8}; len = widths[type];
	} else if(type == TAG_STRING) {
		if(n - *at < 2) return false;
		len = ((size_t)b[*at] << 8) | b[*at + 1]; *at += 2;
	} else if(type == TAG_BYTE_ARRAY || type == TAG_INT_ARRAY || type == TAG_LONG_ARRAY) {
		if(n - *at < 4 || b[*at] & 128) return false;
		uint32_t v = ((uint32_t)b[*at] << 24) | ((uint32_t)b[*at+1] << 16)
			| ((uint32_t)b[*at+2] << 8) | b[*at+3]; *at += 4;
		unsigned width = type == TAG_BYTE_ARRAY ? 1 : type == TAG_INT_ARRAY ? 4 : 8;
		if(v > (n - *at) / width) return false;
		len = (size_t)v * width;
	} else if(type == TAG_LIST) {
		if(n - *at < 5) return false;
		unsigned t = b[(*at)++];
		if(b[*at] & 128) return false;
		uint32_t v = ((uint32_t)b[*at] << 24) | ((uint32_t)b[*at+1] << 16)
			| ((uint32_t)b[*at+2] << 8) | b[*at+3]; *at += 4;
		if(v > n - *at) return false;
		for(uint32_t k = 0; k < v; k++) if(!skip(b,n,at,t,depth+1)) return false;
		return true;
	} else if(type == TAG_COMPOUND) {
		while(*at < n) {
			unsigned t = b[(*at)++]; if(!t) return true;
			if(!skip(b,n,at,TAG_STRING,depth+1) || !skip(b,n,at,t,depth+1)) return false;
		}
		return false;
	} else return false;
	if(len > n - *at) return false;
	*at += len; return true;
}

static bool offsets(struct item_record* r) {
	static const char* names[] = {"id", "Damage", "Count", "Slot"};
	static const unsigned types[] = {TAG_SHORT,TAG_SHORT,TAG_BYTE,TAG_BYTE};
	memset(r->offset, 255, sizeof(r->offset));
	size_t at = 0;
	while(at < r->length) {
		unsigned t = r->bytes[at++];
		if(!t) return at == r->length && r->offset[0] != 255
			&& r->offset[1] != 255 && r->offset[2] != 255;
		if(r->length - at < 2) return false;
		size_t len = ((size_t)r->bytes[at] << 8) | r->bytes[at+1]; at += 2;
		if(len > r->length - at) return false;
		for(unsigned k = 0; k < 4; k++) {
			if(len == strlen(names[k]) && !memcmp(r->bytes+at,names[k],len)) {
				if(t != types[k] || r->offset[k] != 255 || at+len >= 255) return false;
				r->offset[k] = (uint8_t)(at+len);
			}
		}
		at += len;
		if(!skip(r->bytes,r->length,&at,t,0)) return false;
	}
	return false;
}

static size_t canonical(uint8_t* b, const struct item_data* it, int slot) {
	static const uint8_t pattern[] = {1,0,4,'S','l','o','t',0,
		2,0,2,'i','d',0,0,1,0,5,'C','o','u','n','t',0,
		2,0,6,'D','a','m','a','g','e',0,0,0};
	memcpy(b,pattern,sizeof(pattern));
	b[7] = (uint8_t)slot; b[13] = (uint8_t)(it->id>>8); b[14] = (uint8_t)it->id;
	b[23] = it->count; b[33] = (uint8_t)(it->durability>>8); b[34] = (uint8_t)it->durability;
	if(slot < 0) { memmove(b,b+8,sizeof(pattern)-8); return sizeof(pattern)-8; }
	return sizeof(pattern);
}

/* Supported values already live in item_data/the owning slot. Sharing an
 * encoding template across differing counts/IDs/slots is lossless: emit always
 * overlays precisely these bytes. Every unknown byte remains part of the key. */
static bool same_template(const struct item_record* a,const struct item_record* b) {
	if(a->length!=b->length || memcmp(a->offset,b->offset,sizeof(a->offset))) return false;
	for(size_t i=0;i<a->length;i++) {
		bool scalar_byte=false;
		for(unsigned k=0;k<4;k++)
			if(a->offset[k]!=255 && (i==a->offset[k] || (k<2 && i==(size_t)a->offset[k]+1)))
				scalar_byte=true;
		if(!scalar_byte && a->bytes[i]!=b->bytes[i]) return false;
	}
	return true;
}

void item_nbt_capture(const nbt_node* n, struct item_data* it) {
	it->nbt_record = UINT8_MAX;
	struct buffer b = payload(n);
	struct item_record r = {0};
	if(!b.data || b.len > sizeof(r.bytes)) { buffer_free(&b); return; }
	r.length = (uint16_t)b.len; memcpy(r.bytes,b.data,b.len); buffer_free(&b);
	if(!offsets(&r)) return;
	uint8_t plain[36];
	int slot = r.offset[3] == 255 ? -1 : r.bytes[r.offset[3]];
	size_t len = canonical(plain,it,slot);
	if(len == r.length && !memcmp(plain,r.bytes,len)) { it->nbt_record = 0; return; }
	for(unsigned k = 0; k < record_count; k++) {
		if(same_template(&records[k],&r)) {
			it->nbt_record = (uint8_t)(k+1); return;
		}
	}
	if(record_count < ITEM_NBT_RECORDS) {
		records[record_count] = r; it->nbt_record = (uint8_t)(++record_count);
	} else if(item_nbt_exhaustions() != UINT_MAX) {
		/* Archive-owned writer; F3/telemetry read concurrently. */
		__atomic_store_n(&record_exhaustions, item_nbt_exhaustions()+1, __ATOMIC_RELAXED);
	}
}

bool item_nbt_emit(nbt_node* node, const struct item_data* it, int slot) {
	*node = (nbt_node){.type = TAG_RAW_COMPOUND};
	if(item_data_pinned(it)) { node->payload.tag_byte_array.length = -1; return true; }
	uint8_t bytes[ITEM_NBT_RECORD_BYTES+8]; size_t len;
	if(!it->nbt_record) len = canonical(bytes,it,slot);
	else {
		if(it->nbt_record > record_count) return false;
		const struct item_record* r = &records[it->nbt_record-1];
		len = r->length; memcpy(bytes,r->bytes,len);
		uint16_t values[] = {it->id,it->durability,it->count,(uint16_t)slot};
		for(unsigned k = 0; k < 4; k++) {
			if(r->offset[k] == 255) { if(k == 3) continue; return false; }
			if(k < 2) { bytes[r->offset[k]] = (uint8_t)(values[k]>>8); bytes[r->offset[k]+1] = (uint8_t)values[k]; }
			else bytes[r->offset[k]] = (uint8_t)values[k];
		}
		if(r->offset[3]==255 && slot>=0) {
			static const uint8_t header[]={1,0,4,'S','l','o','t',0};
			memmove(bytes+8,bytes,len); memcpy(bytes,header,8);
			bytes[7]=(uint8_t)slot; len+=8;
		} else if(r->offset[3]!=255 && slot<0) {
			size_t start=(size_t)r->offset[3]-7;
			memmove(bytes+start,bytes+start+8,len-start-8); len-=8;
		}
	}
	node->payload.tag_byte_array.data = malloc(len);
	if(!node->payload.tag_byte_array.data) return false;
	memcpy(node->payload.tag_byte_array.data,bytes,len);
	node->payload.tag_byte_array.length = (int32_t)len; return true;
}

static int runtime_slot(unsigned slot, size_t count, bool player) {
	if(player) {
		if(slot >= 100 && slot < 104) slot = 3-(slot-100)+INVENTORY_SLOT_ARMOR;
		else if(slot < 9) slot += INVENTORY_SLOT_HOTBAR;
		else if(slot < 36) slot += INVENTORY_SLOT_MAIN-9;
		else return -1;
	}
	return slot < count ? (int)slot : -1;
}
static int disk_slot(size_t slot, bool player) {
	if(!player) return (int)slot;
	if(slot >= INVENTORY_SLOT_ARMOR && slot < INVENTORY_SLOT_ARMOR+4) return 103-(int)(slot-INVENTORY_SLOT_ARMOR);
	if(slot >= INVENTORY_SLOT_HOTBAR && slot < INVENTORY_SLOT_HOTBAR+9) return (int)(slot-INVENTORY_SLOT_HOTBAR);
	if(slot >= INVENTORY_SLOT_MAIN && slot < INVENTORY_SLOT_MAIN+27) return (int)(slot-INVENTORY_SLOT_MAIN)+9;
	return -1;
}
static int record_slot(const nbt_node* n, size_t count, bool player) {
	if(n && n->type==TAG_RAW_COMPOUND) {
		nbt_node* parsed=materialize(n);
		int slot=record_slot(parsed,count,player); nbt_free(parsed); return slot;
	}
	nbt_node* slot = child(n,"Slot");
	/* ci-1101/41 item 14: Beta reads the slot as getByte("Slot") & 255
	 * (InventoryPlayer.java:229, TileEntityChest.java:56), and NBTTagCompound
	 * .getByte returns 0 for an ABSENT tag, so a Slot-less record lands in
	 * slot 0. A present wrong-type Slot (a ClassCastException in Beta) stays
	 * unplaceable. */
	if(!slot) return n && n->type==TAG_COMPOUND ? runtime_slot(0,count,player) : -1;
	return slot->type == TAG_BYTE ? runtime_slot((uint8_t)slot->payload.tag_byte,count,player) : -1;
}

bool item_nbt_read_list(const nbt_node* list, struct item_data* items, size_t count, bool player) {
	if(count > 256) return false;
	bool seen[256] = {false}, ambiguous = false;
	memset(items,0,count*sizeof(*items));
	if(!list) return true;
	if(list->type != TAG_LIST || !list->payload.tag_list) ambiguous = true;
	else {
		struct list_head* p;
		list_for_each(p,&list->payload.tag_list->entry) {
			nbt_node* n = list_entry(p,struct nbt_list,entry)->data;
			int slot = record_slot(n,count,player);
			/* ci-1101/41 item 14: Beta InventoryPlayer.readFromNBT (RetroMCP
			 * InventoryPlayer.java:226-237) and TileEntityChest.readFromNBT
			 * place a record only inside the slot ranges and otherwise skip
			 * it; they never disable the other slots. The record's bytes are
			 * still kept verbatim by item_nbt_write_list (CaveWii never
			 * deletes what it cannot place). Only a COMPOUND record is
			 * placeable at all; a list of anything else is an unknown shape
			 * and still pins the whole scope as before. */
			if(slot < 0) {
				if(!n || (n->type != TAG_COMPOUND && n->type != TAG_RAW_COMPOUND))
					ambiguous = true;
				continue;
			}
			struct item_data it;
			if(!scalar(n,&it)) it = (struct item_data){.id=1,.count=1,.nbt_record=255};
			else if(!it.id) {
				/* InventoryPlayer.readFromNBT places a stack only when
				 * getItem() != null (InventoryPlayer.java:230); id 0 has no
				 * Item, so the slot stays empty and the record is dropped on
				 * rewrite (Beta's writer emits only non-null stacks).
				 * TileEntityChest.readFromNBT (TileEntityChest.java:56-58) has
				 * no such check and keeps an id-0 stack, which CaveWii cannot
				 * represent (id 0 == empty): a container keeps the pinned,
				 * byte-preserved placeholder it always had. */
				if(player) continue;
				it = (struct item_data){.id=1,.count=1,.nbt_record=255};
			} else if(!it.count) {
				/* A real item with Count 0: Beta places it like any stack
				 * (the getItem() test ignores the count) and writes it back.
				 * CaveWii keeps it in its slot with its own id/damage and
				 * count 0, pinned, so the record is written back byte for
				 * byte and nothing can stack onto a zero-size stack. */
				it.nbt_record = 255;
			} else item_nbt_capture(n,&it);
			if(seen[slot]) it.nbt_record = 255;
			seen[slot] = true; items[slot] = it;
		}
	}
	if(ambiguous) for(size_t k=0;k<count;k++) if(disk_slot(k,player)>=0) {
		if(!items[k].id) items[k] = (struct item_data){.id=1,.count=1};
		items[k].nbt_record = 255;
	}
	return true;
}

static bool append(struct nbt_list* list, nbt_node* n) {
	struct nbt_list* w = malloc(sizeof(*w));
	if(!w || !n) { free(w); nbt_free(n); return false; }
	w->data=n; list_add_tail(&w->entry,&list->entry); return true;
}
static nbt_node* copy_raw(const nbt_node* n) {
	struct buffer b = payload(n);
	if(!b.data) return NULL;
	nbt_node* raw = calloc(1,sizeof(*raw));
	if(!raw) { buffer_free(&b); return NULL; }
	raw->type=TAG_RAW_COMPOUND; raw->payload.tag_byte_array.data=(unsigned char*)b.data;
	raw->payload.tag_byte_array.length=(int32_t)b.len; return raw;
}
nbt_node* item_nbt_clone_lossless(const nbt_node* n) {
	if(!n) return NULL;
	if(n->type==TAG_COMPOUND && n->payload.tag_compound->data
	   && n->payload.tag_compound->data->type==TAG_BYTE_ARRAY) {
		nbt_node* raw=copy_raw(n);
		if(raw && n->name) {
			raw->name=malloc(strlen(n->name)+1);
			if(!raw->name) { nbt_free(raw); return NULL; }
			memcpy(raw->name,n->name,strlen(n->name)+1);
		}
		return raw;
	}
	nbt_node* out=nbt_clone((nbt_node*)n);
	if(!out || (out->type!=TAG_COMPOUND && out->type!=TAG_LIST)) return out;
	struct nbt_list* list=out->type==TAG_LIST ? out->payload.tag_list : out->payload.tag_compound;
	struct list_head* p;
	list_for_each(p,&list->entry) {
		struct nbt_list* w=list_entry(p,struct nbt_list,entry);
		nbt_node* replacement=item_nbt_clone_lossless(w->data);
		if(!replacement) { nbt_free(out); return NULL; }
		nbt_free(w->data); w->data=replacement;
	}
	return out;
}
bool item_nbt_has_retained(const nbt_node* n) {
	if(!n) return false;
	if(n->type==TAG_RAW_COMPOUND) return true;
	if(n->type!=TAG_COMPOUND && n->type!=TAG_LIST) return false;
	struct nbt_list* list=n->type==TAG_LIST ? n->payload.tag_list : n->payload.tag_compound;
	if(!list) return false;
	/* A compound's sentinel `data` is the retained item image; a list's is the
	 * element-type marker, which item_nbt_clone_lossless never rewrites. */
	if(n->type==TAG_COMPOUND && list->data && list->data->type==TAG_BYTE_ARRAY)
		return true;
	struct list_head* p;
	list_for_each(p,&list->entry)
		if(item_nbt_has_retained(list_entry(p,struct nbt_list,entry)->data)) return true;
	return false;
}
bool item_nbt_scope_image(const nbt_node* root, const unsigned char** bytes,
						  size_t* length) {
	if(!root || root->type!=TAG_COMPOUND || !root->payload.tag_compound) return false;
	const nbt_node* raw=root->payload.tag_compound->data;
	if(!raw || raw->type!=TAG_RAW_COMPOUND || raw->payload.tag_byte_array.length<0)
		return false;
	*bytes=raw->payload.tag_byte_array.data;
	*length=(size_t)raw->payload.tag_byte_array.length;
	return true;
}
bool item_nbt_scope_writable(const nbt_node* root) {
	if(!root || root->type!=TAG_COMPOUND || !root->payload.tag_compound) return false;
	nbt_node* raw=root->payload.tag_compound->data;
	if(!raw || raw->type!=TAG_RAW_COMPOUND) return true; /* constructed tree */
	if(raw->payload.tag_byte_array.length <= 0 || !raw->payload.tag_byte_array.data)
		return false;
	struct buffer bytes=nbt_dump_binary_lossless_hint(root,
		(size_t)raw->payload.tag_byte_array.length);
	bool ok=bytes.data && bytes.len==(size_t)raw->payload.tag_byte_array.length
		&& !memcmp(bytes.data,raw->payload.tag_byte_array.data,bytes.len);
	buffer_free(&bytes); return ok;
}
static bool equal(struct item_data a, struct item_data b) {
	return a.id==b.id && a.durability==b.durability && a.count==b.count && a.nbt_record==b.nbt_record;
}

nbt_node* item_nbt_write_list(const nbt_node* original, const struct item_data* items, size_t count, bool player) {
	struct item_data before[256]; bool emitted[256]={false};
	if(!item_nbt_read_list(original,before,count,player)) return NULL;
	bool same=true;
	for(size_t k=0;k<count;k++) if(disk_slot(k,player)>=0 && !equal(before[k],items[k])) same=false;
	if(original && same) return item_nbt_clone_lossless(original);
	if(original && (original->type!=TAG_LIST || !original->payload.tag_list))
		return same ? nbt_clone((nbt_node*)original) : NULL;
	nbt_node* out=calloc(1,sizeof(*out));
	if(!out) return NULL;
	out->type=TAG_LIST; out->payload.tag_list=calloc(1,sizeof(struct nbt_list));
	if(!out->payload.tag_list) { free(out); return NULL; }
	struct nbt_list* list=out->payload.tag_list; INIT_LIST_HEAD(&list->entry);
	list->data=calloc(1,sizeof(nbt_node));
	if(!list->data) { nbt_free(out); return NULL; }
	list->data->type=TAG_COMPOUND;
	if(original) {
		list->data->type=original->payload.tag_list->data->type;
		struct list_head* p;
		list_for_each(p,&original->payload.tag_list->entry) {
			nbt_node* n=list_entry(p,struct nbt_list,entry)->data;
			int slot=record_slot(n,count,player);
			/* ci-1101/41 item 14: an unplaceable record is kept verbatim
			 * whether or not the placed slots changed. */
			if(slot<0) { if(!append(list,nbt_clone(n))) goto fail; continue; }
			if(item_data_pinned(&before[slot])) {
				if(!equal(before[slot],items[slot]) || !append(list,copy_raw(n))) goto fail;
			} else if(!emitted[slot] && items[slot].id) {
				nbt_node* raw=calloc(1,sizeof(*raw));
				if(!raw) goto fail;
				if(!item_nbt_emit(raw,&items[slot],disk_slot((size_t)slot,player))) { free(raw); goto fail; }
				if(item_data_pinned(&items[slot])) { nbt_free(raw); goto fail; }
				if(!append(list,raw)) goto fail;
			}
			emitted[slot]=true;
		}
	}
	for(size_t k=0;k<count;k++) if(!emitted[k] && items[k].id && disk_slot(k,player)>=0) {
		if(item_data_pinned(&items[k])) goto fail;
		nbt_node* raw=calloc(1,sizeof(*raw)); if(!raw) goto fail;
		if(!item_nbt_emit(raw,&items[k],disk_slot(k,player))) { free(raw); goto fail; }
		if(!append(list,raw)) goto fail;
		list->data->type=TAG_COMPOUND;
	}
	return out;
fail:
	nbt_free(out); return NULL;
}
