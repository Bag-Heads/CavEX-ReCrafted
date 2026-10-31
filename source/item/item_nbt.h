#ifndef ITEM_NBT_H
#define ITEM_NBT_H

#include "item_types.h"
typedef struct nbt_node nbt_node;

/* Immutable, interned import templates: 254 distinct encodings/opaque values,
 * at most 256 payload bytes
 * each, process lifetime. No handle recycling and no tick-time allocation.
 * Saturation/oversize/ambiguous records use 255 and require the original scope
 * at save. Handle 0 is the byte-exact canonical {Slot,id,Count,Damage} encoding.
 * [740c583 ItemStack.writeToNBT/readFromNBT; InventoryPlayer.writeToNBT;
 * TileEntityChest/Furnace/Dispenser.writeToNBT: call-order citations only;
 * NBTTagCompound writes HashMap iteration order. Byte fixtures are authoritative]. */
#define ITEM_NBT_RECORD_BYTES 256
#define ITEM_NBT_RECORDS 254

/* Process-lifetime saturation refusals; canonical/deduplicated captures do not increment. */
unsigned item_nbt_exhaustions(void);
unsigned item_nbt_templates(void);

void item_nbt_capture(const nbt_node* compound, struct item_data* item);
/* Initializes a write-only node. Caller frees its byte-array payload. Pinned
 * records emit an invalid length so an unmerged scope cannot be committed. */
bool item_nbt_emit(nbt_node* node, const struct item_data* item, int slot);
bool item_nbt_read_list(const nbt_node* list, struct item_data* items,
					   size_t count, bool player);
/* Newly allocated list; NULL means refuse without changing the original.
 * Old order, unknown fields, duplicate records and pinned bytes are retained. */
nbt_node* item_nbt_write_list(const nbt_node* original,
							const struct item_data* items, size_t count, bool player);
nbt_node* item_nbt_child(const nbt_node* compound, const char* name);
/* Takes ownership of replacement, including on failure. */
bool item_nbt_replace(nbt_node* compound, const char* name, nbt_node* replacement);
bool item_nbt_equal(const nbt_node* a, const nbt_node* b);
nbt_node* item_nbt_clone_lossless(const nbt_node* node);
bool item_nbt_scope_writable(const nbt_node* root);
/* True when any node in the tree carries a retained byte image: the node itself
 * is a TAG_RAW_COMPOUND, or it is a compound whose sentinel holds the
 * TAG_BYTE_ARRAY image nbt_parse retains for an item-shaped compound. That is
 * EXACTLY the set of nodes item_nbt_clone_lossless() rewrites, so when this
 * returns false item_nbt_clone_lossless(n) degenerates to nbt_clone(n) and the
 * caller may keep `n` itself instead -- the serialisation is identical.
 * It is also the "is there anything foreign to preserve here" predicate: a tree
 * with no retained image contains no imported item record and no unknown
 * subtree that the writer would have to carry forward byte-for-byte. */
bool item_nbt_has_retained(const nbt_node* node);
/* Hand back the whole-scope byte image nbt_parse retained on `root` (the
 * TAG_RAW_COMPOUND sentinel), without copying it. False when the tree was
 * constructed rather than parsed. When item_nbt_scope_writable(root) has
 * ALREADY returned true, this image is by definition byte-identical to
 * nbt_dump_binary(item_nbt_clone_lossless(root)) -- that is precisely what
 * item_nbt_scope_writable proved -- so a caller holding that certificate can
 * compare against these bytes instead of materialising the lossless clone. */
bool item_nbt_scope_image(const nbt_node* root, const unsigned char** bytes,
						  size_t* length);

#endif
