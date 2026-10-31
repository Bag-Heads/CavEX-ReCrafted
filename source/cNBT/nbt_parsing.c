/*
 * -----------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42):
 * Lukas Niederbremer <webmaster@flippeh.de> and Clark Gaebel <cg.wowus.cg@gmail.com>
 * wrote this file. As long as you retain this notice you can do whatever you
 * want with this stuff. If we meet some day, and you think this stuff is worth
 * it, you can buy us a beer in return.
 * -----------------------------------------------------------------------------
 */
#include "nbt.h"

#include "buffer.h"
#include "list.h"

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* are we running on a little-endian system? */
static int little_endian()
{
    uint16_t t = 0x0001;
    char c[2];
    memcpy(c, &t, sizeof t);
    return c[0];
}

static void* swap_bytes(void* s, size_t len)
{
    for(char* b = s,
            * e = b + len - 1;
        b < e;
        b++, e--)
    {
        char t = *b;

        *b = *e;
        *e = t;
    }

    return s;
}

/* big endian to native endian. works in-place */
static void* be2ne(void* s, size_t len)
{
    return little_endian() ? swap_bytes(s, len) : s;
}

/* native endian to big endian. works the exact same as its inverse */
#define ne2be be2ne

/* A special form of memcpy which copies `n' bytes into `dest', then returns
 * `src' + n.
 */
static const void* memscan(void* dest, const void* src, size_t n)
{
    memcpy(dest, src, n);
    return (const char*)src + n;
}

/* Does a memscan, then goes from big endian to native endian on the
 * destination.
 */
static const void* swapped_memscan(void* dest, const void* src, size_t n)
{
    const void* ret = memscan(dest, src, n);
    return be2ne(dest, n), ret;
}

#define CHECKED_MALLOC(var, n, on_error) do { \
    if((var = malloc(n)) == NULL)             \
    {                                         \
        errno = NBT_EMEM;                     \
        on_error;                             \
    }                                         \
} while(0)

#define CHECKED_APPEND(b, ptr, len) do { \
    if(buffer_append((b), (ptr), (len))) \
        return NBT_EMEM;                 \
} while(0)

/* Parses a tag, given a name (may be NULL) and a type. Fills in the payload. */
static nbt_node* parse_unnamed_tag(nbt_type type, char* name, const char** memory, size_t* length);

/* A valid CaveWii chunk stays far below these limits. They bound recursive
 * stack use and aggregate heap nodes for hostile/corrupt NBT. Parsing is
 * single-thread owned by the archive layer. */
#define NBT_PARSE_MAX_DEPTH 64
#define NBT_PARSE_MAX_NODES 131072
static size_t s_parse_depth;
static size_t s_parse_nodes;
/* Additional raw-retention heap is bounded independently of node count.
 * Exceeding this budget rejects the parse transaction, never a partial scope. */
#define NBT_PARSE_RAW_BYTES_MAX (8u * 1024u * 1024u)
static size_t s_parse_raw_bytes;

static nbt_node* retain_raw(const void* bytes, size_t length, nbt_type marker) {
    if(length > INT32_MAX || length > NBT_PARSE_RAW_BYTES_MAX - sizeof(nbt_node)
       || s_parse_raw_bytes > NBT_PARSE_RAW_BYTES_MAX - sizeof(nbt_node) - length)
        { errno = NBT_EREFUSED; return NULL; }
    nbt_node* raw = calloc(1, sizeof(*raw) + length);
    if(!raw) { errno = NBT_EMEM; return NULL; }
    raw->type = marker;
    raw->payload.tag_byte_array.length = (int32_t)length;
    raw->payload.tag_byte_array.data = (unsigned char*)(raw + 1);
    memcpy(raw + 1, bytes, length);
    s_parse_raw_bytes += sizeof(*raw) + length;
    return raw;
}


/*
 * Reads some bytes from the memory stream. This macro will read `n'
 * bytes into `dest', call either memscan or swapped_memscan depending on
 * `scanner', then fix the length. If anything funky goes down, `on_failure'
 * will be executed.
 */
#define READ_GENERIC(dest, n, scanner, on_failure) do { \
    if(*length < (n)) { on_failure; }                   \
    *memory = scanner((dest), *memory, (n));            \
    *length -= (n);                                     \
} while(0)

/* printfs into the end of a buffer. Note: no null-termination! */
static void bprintf(struct buffer* b, const char* restrict format, ...)
{
    va_list args;
    int siz;

    va_start(args, format);
    siz = vsnprintf(NULL, 0, format, args);
    va_end(args);

    buffer_reserve(b, b->len + siz + 1);

    va_start(args, format);
    vsnprintf((char*)(b->data + b->len), siz + 1, format, args);
    va_end(args);

    b->len += siz; // remember - no null terminator!
}

/*
 * Reads a string from memory, moving the pointer and updating the length
 * appropriately. Returns NULL on failure.
 */
static char* read_string(const char** memory, size_t* length)
{
    int16_t string_length;
    char* ret = NULL;

    READ_GENERIC(&string_length, sizeof string_length, swapped_memscan, goto parse_error);

    if(string_length < 0)               goto parse_error;
    if(*length < (size_t)string_length) goto parse_error;

    CHECKED_MALLOC(ret, string_length + 1, goto parse_error);

    READ_GENERIC(ret, (size_t)string_length, memscan, goto parse_error);

    ret[string_length] = '\0'; /* don't forget to NULL-terminate ;) */
    return ret;

parse_error:
    if(errno == NBT_OK)
        errno = NBT_ERR;

    free(ret);
    return NULL;
}

static nbt_node* parse_named_tag(const char** memory, size_t* length)
{
  char* name = NULL;

  uint8_t type;
  READ_GENERIC(&type, sizeof type, memscan, goto parse_error);

  name = read_string(memory, length);
  if(name == NULL) goto parse_error;

  nbt_node* ret = parse_unnamed_tag((nbt_type)type, name, memory, length);
  if(ret == NULL) goto parse_error;

  return ret;

parse_error:
  if(errno == NBT_OK)
    errno = NBT_ERR;

  free(name);
  return NULL;
}

static struct nbt_byte_array read_byte_array(const char** memory, size_t* length)
{
    struct nbt_byte_array ret = {0};

    READ_GENERIC(&ret.length, sizeof ret.length, swapped_memscan, goto parse_error);

    if(ret.length < 0 || (size_t)ret.length > *length) goto parse_error;

    if(ret.length > 0)
    {
        CHECKED_MALLOC(ret.data, (size_t)ret.length, goto parse_error);
        READ_GENERIC(ret.data, (size_t)ret.length, memscan, goto parse_error);
    }

    return ret;

parse_error:
    if(errno == NBT_OK)
        errno = NBT_ERR;

    free(ret.data);
    ret.data = NULL;
    return ret;
}

static struct nbt_int_array read_int_array(const char** memory, size_t* length)
{
    struct nbt_int_array ret = {0};

    READ_GENERIC(&ret.length, sizeof ret.length, swapped_memscan, goto parse_error);

    if(ret.length < 0
       || (size_t)ret.length > *length / sizeof(int32_t)) goto parse_error;

    if(ret.length > 0)
    {
        CHECKED_MALLOC(ret.data, (size_t)ret.length * sizeof(int32_t), goto parse_error);
        READ_GENERIC(ret.data, (size_t)ret.length * sizeof(int32_t), memscan, goto parse_error);
    }


    // Byteswap the whole array.
    for(int32_t i = 0; i < ret.length; i++)
        be2ne(ret.data + i, sizeof(int32_t));

    return ret;

parse_error:
    if(errno == NBT_OK)
        errno = NBT_ERR;

    free(ret.data);
    ret.data = NULL;
    return ret;
}

static struct nbt_long_array read_long_array(const char** memory, size_t* length)
{
    struct nbt_long_array ret = {0};

    READ_GENERIC(&ret.length, sizeof ret.length, swapped_memscan, goto parse_error);

    if(ret.length < 0
       || (size_t)ret.length > *length / sizeof(int64_t)) goto parse_error;

    if(ret.length > 0)
    {
        CHECKED_MALLOC(ret.data, (size_t)ret.length * sizeof(int64_t), goto parse_error);
        READ_GENERIC(ret.data, (size_t)ret.length * sizeof(int64_t), memscan, goto parse_error);
    }


    // Byteswap the whole array.
    for(int32_t i = 0; i < ret.length; i++)
        be2ne(ret.data + i, sizeof(int64_t));

    return ret;

parse_error:
    if(errno == NBT_OK)
        errno = NBT_ERR;

    free(ret.data);
    ret.data = NULL;
    return ret;
}

/*
 * Is the list all one type? If yes, return the type. Otherwise, return
 * TAG_INVALID
 */
static nbt_type list_is_homogenous(const struct nbt_list* list)
{
    nbt_type type = TAG_INVALID;

    const struct list_head* pos;
    list_for_each(pos, &list->entry)
    {
        const struct nbt_list* cur = list_entry(pos, const struct nbt_list, entry);

        assert(cur->data);
        assert(cur->data->type != TAG_INVALID);

        if(cur->data->type == TAG_INVALID)
            return TAG_INVALID;

        /* if we're the first type, just set it to our current type */
        nbt_type wire_type = cur->data->type == TAG_RAW_COMPOUND ? TAG_COMPOUND : cur->data->type;
        if(type == TAG_INVALID) type = wire_type;

        if(type != wire_type)
            return TAG_INVALID;
    }

    /* if the list was empty, use the sentinel type */
    if(type == TAG_INVALID && list->data != NULL)
        type = list->data->type;

    return type;
}

static struct nbt_list* read_list(const char** memory, size_t* length)
{
    uint8_t type;
    int32_t elems;
    struct nbt_list* ret = NULL;

    CHECKED_MALLOC(ret, sizeof *ret, goto parse_error);

    /* Initialize BEFORE the second malloc: if it fails, the parse_error
     * unwind hands this node to nbt_free_list, which would otherwise walk an
     * uninitialized list head (wild pointers) and free(garbage data). With
     * data=NULL + an initialized head, the unwind is a clean no-op free. */
    ret->data = NULL;
    INIT_LIST_HEAD(&ret->entry);

    /* we allocate the data pointer to store the type of the list in the first
     * sentinel element */
    CHECKED_MALLOC(ret->data, sizeof *ret->data, goto parse_error);

    READ_GENERIC(&type, sizeof type, swapped_memscan, goto parse_error);
    READ_GENERIC(&elems, sizeof elems, swapped_memscan, goto parse_error);

    if(elems < 0) goto parse_error;
    if((size_t)elems > NBT_PARSE_MAX_NODES)
        { errno = NBT_EREFUSED; goto parse_error; }

    ret->data->type = type == TAG_INVALID ? TAG_COMPOUND : (nbt_type)type;

    for(int32_t i = 0; i < elems; i++)
    {
        struct nbt_list* new;

        CHECKED_MALLOC(new, sizeof *new, goto parse_error);

        new->data = parse_unnamed_tag((nbt_type)type, NULL, memory, length);

        if(new->data == NULL)
        {
            free(new);
            goto parse_error;
        }

        list_add_tail(&new->entry, &ret->entry);
    }

    return ret;

parse_error:
    if(errno == NBT_OK)
        errno = NBT_ERR;

    nbt_free_list(ret);
    return NULL;
}

static struct nbt_list* read_compound(const char** memory, size_t* length)
{
    struct nbt_list* ret = NULL;

    CHECKED_MALLOC(ret, sizeof *ret, goto parse_error);

    ret->data = NULL;
    INIT_LIST_HEAD(&ret->entry);

    for(;;)
    {
        uint8_t type;
        char* name = NULL;
        struct nbt_list* new_entry;

        READ_GENERIC(&type, sizeof type, swapped_memscan, goto parse_error);

        if(type == 0) break; /* TAG_END == 0. We've hit the end of the list when type == TAG_END. */

        name = read_string(memory, length);
        if(name == NULL) goto parse_error;

        CHECKED_MALLOC(new_entry, sizeof *new_entry,
            free(name);
            goto parse_error;
        );

        new_entry->data = parse_unnamed_tag((nbt_type)type, name, memory, length);

        if(new_entry->data == NULL)
        {
            free(new_entry);
            free(name);
            goto parse_error;
        }

        list_add_tail(&new_entry->entry, &ret->entry);
    }

    return ret;

parse_error:
    if(errno == NBT_OK)
        errno = NBT_ERR;
    nbt_free_list(ret);

    return NULL;
}

/*
 * Parses a tag, given a name (may be NULL) and a type. Fills in the payload.
 */
static nbt_node* parse_unnamed_tag(nbt_type type, char* name, const char** memory, size_t* length)
{
    const char* original_payload = *memory;
    bool nested = type == TAG_LIST || type == TAG_COMPOUND;
    bool entered_nested = false;
    nbt_node* node = NULL;

    if(s_parse_nodes >= NBT_PARSE_MAX_NODES)
        { errno = NBT_EREFUSED; goto parse_error; }
    s_parse_nodes++;
    if(nested)
    {
        if(s_parse_depth >= NBT_PARSE_MAX_DEPTH)
            { errno = NBT_EREFUSED; goto parse_error; }
        s_parse_depth++;
        entered_nested = true;
    }

    CHECKED_MALLOC(node, sizeof *node, goto parse_error);

    node->type = type;
    node->name = name;

#define COPY_INTO_PAYLOAD(payload_name) \
    READ_GENERIC(&node->payload.payload_name, sizeof node->payload.payload_name, swapped_memscan, goto parse_error);

    switch(type)
    {
    case TAG_BYTE:
        COPY_INTO_PAYLOAD(tag_byte);
        break;
    case TAG_SHORT:
        COPY_INTO_PAYLOAD(tag_short);
        break;
    case TAG_INT:
        COPY_INTO_PAYLOAD(tag_int);
        break;
    case TAG_LONG:
        COPY_INTO_PAYLOAD(tag_long);
        break;
    case TAG_FLOAT:
        COPY_INTO_PAYLOAD(tag_float);
        break;
    case TAG_DOUBLE:
        COPY_INTO_PAYLOAD(tag_double);
        break;
    case TAG_BYTE_ARRAY:
        node->payload.tag_byte_array = read_byte_array(memory, length);
        break;
    case TAG_INT_ARRAY:
        node->payload.tag_int_array = read_int_array(memory, length);
        break;
    case TAG_LONG_ARRAY:
        node->payload.tag_long_array = read_long_array(memory, length);
        break;
    case TAG_STRING:
        node->payload.tag_string = read_string(memory, length);
        break;
    case TAG_LIST:
        node->payload.tag_list = read_list(memory, length);
        break;
    case TAG_COMPOUND:
        node->payload.tag_compound = read_compound(memory, length);
        break;

    default:
        goto parse_error; /* Unknown node or TAG_END. Either way, we shouldn't be parsing this. */
    }

#undef COPY_INTO_PAYLOAD

    if(errno != NBT_OK) goto parse_error;

    /* Keep original item compound payloads, including unknown tags and their
     * exact encoding. The otherwise unused compound sentinel data owns one
     * allocation (node + trailing bytes), freed by nbt_free_list. */
    if(type == TAG_COMPOUND) {
        bool item = false;
        struct list_head* p;
        list_for_each(p, &node->payload.tag_compound->entry) {
            nbt_node* child = list_entry(p, struct nbt_list, entry)->data;
            if(child->name && (!strcmp(child->name, "Slot")
                || (!strcmp(child->name, "id") && child->type == TAG_SHORT)))
                item = true;
        }
        if(item) {
            size_t bytes = (size_t)(*memory - original_payload);
            nbt_node* raw = retain_raw(original_payload, bytes, TAG_BYTE_ARRAY);
            if(!raw) {
                nbt_free_list(node->payload.tag_compound);
                goto parse_error;
            }
            node->payload.tag_compound->data = raw;
        }
    }

    if(entered_nested) s_parse_depth--;
    return node;

parse_error:
    if(errno == NBT_OK)
        errno = NBT_ERR;

    if(entered_nested) s_parse_depth--;
    free(node);
    return NULL;
}

nbt_node* nbt_parse(const void* mem, size_t len)
{
    const void* original = mem;
    size_t original_length = len;
    errno = NBT_OK;

    s_parse_depth = 0;
    s_parse_nodes = 0;
    s_parse_raw_bytes = 0;

    const char** memory = (const char**)&mem;
    size_t* length = &len;

    nbt_node* root = parse_named_tag(memory, length);
    /* Keep one full scope image for archive write preflight. Unlike item
     * snapshots (BYTE_ARRAY), RAW_COMPOUND sentinel metadata is never copied
     * into a replacement tree or emitted automatically. */
    if(root && root->type == TAG_COMPOUND && !root->payload.tag_compound->data) {
        nbt_node* raw = retain_raw(original, original_length, TAG_RAW_COMPOUND);
        if(!raw) { int status = errno; nbt_free(root); errno = status; return NULL; }
        root->payload.tag_compound->data = raw;
    }
    return root;
}

/* spaces, not tabs ;) */
static void indent(struct buffer* b, size_t amount)
{
    size_t spaces = amount * 4; /* 4 spaces per indent */

    char temp[spaces + 1];

    for(size_t i = 0; i < spaces; ++i)
        temp[i] = ' ';
    temp[spaces] = '\0';

    bprintf(b, "%s", temp);
}

static nbt_status __nbt_dump_ascii(const nbt_node*, struct buffer*, size_t ident);

/* prints the node's name, or (null) if it has none. */
#define SAFE_NAME(node) ((node)->name ? (node)->name : "<null>")

static void dump_byte_array(const struct nbt_byte_array ba, struct buffer* b)
{
    assert(ba.length >= 0);

    bprintf(b, "[ ");
    for(int32_t i = 0; i < ba.length; ++i)
        bprintf(b, "%u ", +ba.data[i]);
    bprintf(b, "]");
}

static void dump_int_array(const struct nbt_int_array ia, struct buffer* b)
{
    assert(ia.length >= 0);

    bprintf(b, "[ ");
    for(int32_t i = 0; i < ia.length; ++i)
        bprintf(b, "%u ", +ia.data[i]);
    bprintf(b, "]");
}

static void dump_long_array(const struct nbt_long_array la, struct buffer* b)
{
    assert(la.length >= 0);

    bprintf(b, "[ ");
    for(int32_t i = 0; i < la.length; ++i)
        bprintf(b, "%u ", +la.data[i]);
    bprintf(b, "]");
}

static nbt_status dump_list_contents_ascii(const struct nbt_list* list, struct buffer* b, size_t ident)
{
    const struct list_head* pos;

    list_for_each(pos, &list->entry)
    {
        const struct nbt_list* entry = list_entry(pos, const struct nbt_list, entry);
        nbt_status err;

        if((err = __nbt_dump_ascii(entry->data, b, ident)) != NBT_OK)
            return err;
    }

    return NBT_OK;
}

static nbt_status __nbt_dump_ascii(const nbt_node* tree, struct buffer* b, size_t ident)
{
    if(tree == NULL) return NBT_OK;

    indent(b, ident);

    if(tree->type == TAG_BYTE)
        bprintf(b, "TAG_Byte(\"%s\"): %i\n", SAFE_NAME(tree), (int)tree->payload.tag_byte);
    else if(tree->type == TAG_SHORT)
        bprintf(b, "TAG_Short(\"%s\"): %i\n", SAFE_NAME(tree), (int)tree->payload.tag_short);
    else if(tree->type == TAG_INT)
        bprintf(b, "TAG_Int(\"%s\"): %i\n", SAFE_NAME(tree), (int)tree->payload.tag_int);
    else if(tree->type == TAG_LONG)
        bprintf(b, "TAG_Long(\"%s\"): %" PRIi64 "\n", SAFE_NAME(tree), tree->payload.tag_long);
    else if(tree->type == TAG_FLOAT)
        bprintf(b, "TAG_Float(\"%s\"): %f\n", SAFE_NAME(tree), (double)tree->payload.tag_float);
    else if(tree->type == TAG_DOUBLE)
        bprintf(b, "TAG_Double(\"%s\"): %f\n", SAFE_NAME(tree), tree->payload.tag_double);
    else if(tree->type == TAG_BYTE_ARRAY)
    {
        bprintf(b, "TAG_Byte_Array(\"%s\"): ", SAFE_NAME(tree));
        dump_byte_array(tree->payload.tag_byte_array, b);
        bprintf(b, "\n");
    }
    else if(tree->type == TAG_INT_ARRAY)
    {
        bprintf(b, "Tag_Int_Array(\"%s\"): ", SAFE_NAME(tree));
        dump_int_array(tree->payload.tag_int_array, b);
        bprintf(b, "\n");
    }
    else if(tree->type == TAG_LONG_ARRAY)
    {
        bprintf(b, "Tag_Long_Array(\"%s\"): ", SAFE_NAME(tree));
        dump_long_array(tree->payload.tag_long_array, b);
        bprintf(b, "\n");
    }
    else if(tree->type == TAG_STRING)
    {
        if(tree->payload.tag_string == NULL)
            return NBT_ERR;

        bprintf(b, "TAG_String(\"%s\"): %s\n", SAFE_NAME(tree), tree->payload.tag_string);
    }
    else if(tree->type == TAG_LIST)
    {
        bprintf(b, "TAG_List(\"%s\") [%s]\n", SAFE_NAME(tree), nbt_type_to_string(tree->payload.tag_list->data->type));
        indent(b, ident);
        bprintf(b, "{\n");

        nbt_status err = dump_list_contents_ascii(tree->payload.tag_list, b, ident + 1);

        indent(b, ident);
        bprintf(b, "}\n");

        if(err != NBT_OK)
            return err;
    }
    else if(tree->type == TAG_COMPOUND)
    {
        bprintf(b, "TAG_Compound(\"%s\")\n", SAFE_NAME(tree));
        indent(b, ident);
        bprintf(b, "{\n");

        nbt_status err = dump_list_contents_ascii(tree->payload.tag_compound, b, ident + 1);

        indent(b, ident);
        bprintf(b, "}\n");

        if(err != NBT_OK)
            return err;
    }

    else
        return NBT_ERR;

    return NBT_OK;
}

char* nbt_dump_ascii(const nbt_node* tree)
{
    errno = NBT_OK;

    assert(tree);

    struct buffer b = BUFFER_INIT;

    if((errno = __nbt_dump_ascii(tree, &b, 0)) != NBT_OK) goto OOM;
    if(         buffer_reserve(&b, b.len + 1))            goto OOM;

    b.data[b.len] = '\0'; /* null-terminate that biatch, since bprintf doesn't
                             do that for us. */

    return (char*)b.data;

OOM:
    if(errno != NBT_OK)
        errno = NBT_EMEM;

    buffer_free(&b);
    return NULL;
}

static nbt_status dump_byte_array_binary(const struct nbt_byte_array ba, struct buffer* b)
{
    int32_t dumped_length = ba.length;

    ne2be(&dumped_length, sizeof dumped_length);

    CHECKED_APPEND(b, &dumped_length, sizeof dumped_length);

    if(ba.length) assert(ba.data);

    CHECKED_APPEND(b, ba.data, ba.length);

    return NBT_OK;
}

static nbt_status dump_int_array_binary(const struct nbt_int_array ia, struct buffer* b)
{
    int32_t dumped_length = ia.length;

    ne2be(&dumped_length, sizeof dumped_length);

    CHECKED_APPEND(b, &dumped_length, sizeof dumped_length);

    if(ia.length) assert(ia.data);

    for(int32_t i = 0; i < ia.length; i++)
    {
        int32_t swappedElem = ia.data[i];
        ne2be(&swappedElem, sizeof(swappedElem));
        CHECKED_APPEND(b, &swappedElem, sizeof(swappedElem));
    }

    return NBT_OK;
}

static nbt_status dump_long_array_binary(const struct nbt_long_array la, struct buffer* b)
{
    int32_t dumped_length = la.length;

    ne2be(&dumped_length, sizeof dumped_length);

    CHECKED_APPEND(b, &dumped_length, sizeof dumped_length);

    if(la.length) assert(la.data);

    for(int32_t i = 0; i < la.length; i++)
    {
        int64_t swappedElem = la.data[i];
        ne2be(&swappedElem, sizeof(swappedElem));
        CHECKED_APPEND(b, &swappedElem, sizeof(swappedElem));
    }

    return NBT_OK;
}

static nbt_status dump_string_binary(const char* name, struct buffer* b)
{
    assert(name);

    size_t len = strlen(name);

    if(len > 32767 /* SHORT_MAX */)
        return NBT_ERR;

    { /* dump the length */
        int16_t dumped_len = (int16_t)len;
        ne2be(&dumped_len, sizeof dumped_len);

        CHECKED_APPEND(b, &dumped_len, sizeof dumped_len);
    }

    CHECKED_APPEND(b, name, len);

    return NBT_OK;
}

static nbt_status __dump_binary(const nbt_node*, bool, struct buffer*, bool);

static nbt_status dump_list_binary(const struct nbt_list* list, struct buffer* b, bool lossless)
{
    nbt_type type = list_is_homogenous(list);

    size_t len = list_length(&list->entry);

    if(len > 2147483647 /* INT_MAX */)
        return NBT_ERR;

    assert(type != TAG_INVALID);

    if(type == TAG_INVALID)
        return NBT_ERR;

    {
        int8_t _type = (int8_t)type;
        ne2be(&_type, sizeof _type); /* unnecessary, but left in to keep similar code looking similar */
        CHECKED_APPEND(b, &_type, sizeof _type);
    }

    {
        int32_t dumped_len = (int32_t)len;
        ne2be(&dumped_len, sizeof dumped_len);
        CHECKED_APPEND(b, &dumped_len, sizeof dumped_len);
    }

    const struct list_head* pos;
    list_for_each(pos, &list->entry)
    {
        const struct nbt_list* entry = list_entry(pos, const struct nbt_list, entry);
        nbt_status ret;

        if((ret = __dump_binary(entry->data, false, b, lossless)) != NBT_OK)
            return ret;
    }

    return NBT_OK;
}

static nbt_status dump_compound_binary(const struct nbt_list* list, struct buffer* b, bool lossless)
{
    const struct list_head* pos;
    list_for_each(pos, &list->entry)
    {
        const struct nbt_list* entry = list_entry(pos, const struct nbt_list, entry);
        nbt_status ret;

        if((ret = __dump_binary(entry->data, true, b, lossless)) != NBT_OK)
            return ret;
    }

    /* write out TAG_End */
    uint8_t zero = 0;
    CHECKED_APPEND(b, &zero, sizeof zero);

    return NBT_OK;
}

/*
 * @param dump_type   Should we dump the type, or just skip it? We need to skip
 *                    when dumping lists, because the list header already says
 *                    the type.
 */
static nbt_status __dump_binary(const nbt_node* tree, bool dump_type, struct buffer* b, bool lossless)
{
    if(dump_type)
    { /* write out the type */
        int8_t type = tree->type == TAG_RAW_COMPOUND ? TAG_COMPOUND : (int8_t)tree->type;

        CHECKED_APPEND(b, &type, sizeof type);
    }

    if(tree->name)
    {
        nbt_status err;

        if((err = dump_string_binary(tree->name, b)) != NBT_OK)
            return err;
    }

#define DUMP_NUM(type, x) do {               \
    type temp = x;                           \
    ne2be(&temp, sizeof temp);               \
    CHECKED_APPEND(b, &temp, sizeof temp);   \
} while(0)

    /* Same substitution as item_nbt_clone_lossless: an item compound's
     * BYTE_ARRAY sentinel carries its exact original body, including End.
     * Serialize that body directly without cloning the rest of the tree.
     * The root's RAW_COMPOUND scope-image sentinel is deliberately ignored,
     * exactly as in clone_lossless, so current outer fields are still dumped. */
    if(lossless && tree->type == TAG_COMPOUND && tree->payload.tag_compound->data
       && tree->payload.tag_compound->data->type == TAG_BYTE_ARRAY) {
        const struct nbt_byte_array raw = tree->payload.tag_compound->data->payload.tag_byte_array;
        if(raw.length <= 0 || !raw.data) return NBT_ERR;
        CHECKED_APPEND(b, raw.data, (size_t)raw.length);
        return NBT_OK;
    }

    if(tree->type == TAG_RAW_COMPOUND) {
        if(tree->payload.tag_byte_array.length < 0) return NBT_ERR;
        if(lossless && (tree->payload.tag_byte_array.length == 0
                       || !tree->payload.tag_byte_array.data)) return NBT_ERR;
        CHECKED_APPEND(b, tree->payload.tag_byte_array.data,
                       (size_t)tree->payload.tag_byte_array.length);
    }
    else if(tree->type == TAG_BYTE)
        DUMP_NUM(int8_t, tree->payload.tag_byte);
    else if(tree->type == TAG_SHORT)
        DUMP_NUM(int16_t, tree->payload.tag_short);
    else if(tree->type == TAG_INT)
        DUMP_NUM(int32_t, tree->payload.tag_int);
    else if(tree->type == TAG_LONG)
        DUMP_NUM(int64_t, tree->payload.tag_long);
    else if(tree->type == TAG_FLOAT)
        DUMP_NUM(float, tree->payload.tag_float);
    else if(tree->type == TAG_DOUBLE)
        DUMP_NUM(double, tree->payload.tag_double);
    else if(tree->type == TAG_BYTE_ARRAY)
        return dump_byte_array_binary(tree->payload.tag_byte_array, b);
    else if(tree->type == TAG_INT_ARRAY)
        return dump_int_array_binary(tree->payload.tag_int_array, b);
    else if(tree->type == TAG_LONG_ARRAY)
        return dump_long_array_binary(tree->payload.tag_long_array, b);
    else if(tree->type == TAG_STRING)
        return dump_string_binary(tree->payload.tag_string, b);
    else if(tree->type == TAG_LIST)
        return dump_list_binary(tree->payload.tag_list, b, lossless);
    else if(tree->type == TAG_COMPOUND)
        return dump_compound_binary(tree->payload.tag_compound, b, lossless);

    else
        return NBT_ERR;

    return NBT_OK;

#undef DUMP_NUM
}

static struct buffer dump_binary_hint(const nbt_node* tree, size_t size_hint, bool lossless)
{
    errno = NBT_OK;

    if(tree == NULL) return BUFFER_INIT;

    struct buffer ret = BUFFER_INIT;

    /* T1 (2026-09-01, source/cNBT/buffer.c): a caller that already knows
     * roughly how large the dumped payload will be (e.g. a full chunk's
     * fixed-size byte arrays) can pre-size the buffer's single backing
     * allocation in ONE malloc instead of letting lazy_init's 1KB start
     * double its way up -- ~18 realloc+copy cycles to reach ~82KB. A
     * failed reserve here leaves `ret` as the untouched BUFFER_INIT
     * (buffer_reserve_exact's own degrade contract), so a bad/oversized
     * hint can never turn a dump that would otherwise have succeeded into
     * a hard failure -- __dump_binary's first append just lazy_inits at
     * the default size instead. size_hint == 0 (every existing call site,
     * via nbt_dump_binary below) reproduces the exact prior behaviour
     * byte-for-byte -- this is purely an allocation-shape change, not a
     * codec change. */
    if(size_hint > 0)
        buffer_reserve_exact(&ret, size_hint);

    errno = __dump_binary(tree, true, &ret, lossless);
    if(lossless && errno != NBT_OK) buffer_free(&ret);

    return ret;
}

struct buffer nbt_dump_binary_hint(const nbt_node* tree, size_t size_hint)
{
    return dump_binary_hint(tree, size_hint, false);
}

struct buffer nbt_dump_binary_lossless_hint(const nbt_node* tree, size_t size_hint)
{
    return dump_binary_hint(tree, size_hint, true);
}

struct buffer nbt_dump_binary(const nbt_node* tree)
{
    return nbt_dump_binary_hint(tree, 0);
}
