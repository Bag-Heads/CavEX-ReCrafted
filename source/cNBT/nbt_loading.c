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
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/*
 * zlib resources:
 *
 * http://zlib.net/manual.html
 * http://zlib.net/zlib_how.html
 * http://www.gzip.org/zlib/zlib_faq.html
 */

/* The number of bytes to process at a time */
#define CHUNK_SIZE 4096

/* Save files are tiny compared with this ceiling (a chunk is normally below
 * 256 KiB uncompressed). Bound both sides so a corrupt gzip/zlib stream cannot
 * consume the Wii heap before the structural parser gets a chance to reject it. */
#define NBT_MAX_COMPRESSED_BYTES   (8u * 1024u * 1024u)
#define NBT_MAX_DECOMPRESSED_BYTES (8u * 1024u * 1024u)

/*
 * Reads a whole file into a buffer. Returns a NULL buffer and sets errno on
 * error.
 */
static struct buffer read_file(FILE* fp)
{
    struct buffer ret = BUFFER_INIT;

    size_t bytes_read;

    do {
        if(ret.len >= NBT_MAX_COMPRESSED_BYTES)
        {
            int extra = fgetc(fp);
            if(extra == EOF && !ferror(fp)) break;
            return (errno = ferror(fp) ? NBT_EIO : NBT_EREFUSED), buffer_free(&ret), BUFFER_INIT;
        }

        size_t request = CHUNK_SIZE;
        if(request > NBT_MAX_COMPRESSED_BYTES - ret.len)
            request = NBT_MAX_COMPRESSED_BYTES - ret.len;

        if(buffer_reserve(&ret, ret.len + request))
            return (errno = NBT_EMEM), buffer_free(&ret), BUFFER_INIT;

        bytes_read = fread(ret.data + ret.len, 1, request, fp);
        ret.len += bytes_read;

        if(ferror(fp))
            return (errno = NBT_EIO), buffer_free(&ret), BUFFER_INIT;
        if(bytes_read < request && !feof(fp))
            return (errno = NBT_EIO), buffer_free(&ret), BUFFER_INIT;

    } while(!feof(fp));

    return ret;
}

static nbt_status write_file(FILE* fp, const void* data, size_t len)
{
    const char* cdata = data;
    size_t bytes_left = len;

    size_t bytes_written;

    do {
        bytes_written = fwrite(cdata, 1, bytes_left, fp);
        if(ferror(fp)) return NBT_EIO;
        if(bytes_written == 0) return NBT_EIO;

        bytes_left -= bytes_written;
        cdata      += bytes_written;

    } while(bytes_left > 0);

    return NBT_OK;
}

/*
 * Reads in uncompressed data and returns a buffer with the $(strat)-compressed
 * data within. Returns a NULL buffer on failure, and sets errno appropriately.
 */
static struct buffer __compress(const void* mem,
                                size_t len,
                                nbt_compression_strategy strat)
{
    struct buffer ret = BUFFER_INIT;

    errno = NBT_OK;

    if((mem == NULL && len != 0) || len > UINT_MAX)
        return (errno = NBT_ERR), BUFFER_INIT;

    z_stream stream = {
        .zalloc   = Z_NULL,
        .zfree    = Z_NULL,
        .opaque   = Z_NULL,
        .next_in  = (void*)mem,
        .avail_in = len
    };

    /* "The default value is 15"... */
    int windowbits = 15;

    /* ..."Add 16 to windowBits to write a simple gzip header and trailer around
     * the compressed data instead of a zlib wrapper." */
    if(strat == STRAT_GZIP)
        windowbits += 16;

    if(deflateInit2(&stream,
                    Z_DEFAULT_COMPRESSION,
                    Z_DEFLATED,
                    windowbits,
                    8,
                    Z_DEFAULT_STRATEGY
                   ) != Z_OK)
    {
        errno = NBT_EZ;
        return BUFFER_INIT;
    }

    /* CaveWii: pin the gzip header's OS byte (RFC 1952 offset 9) to 0. Without
     * a header zlib writes its compile-time OS_CODE there -- 0x0a from a Win32
     * zlib, 0x03 from the Unix/devkitPPC one -- so the same level.dat was not
     * byte-identical across PC, Linux and the Wii. Beta 1.7.3 wrote level.dat
     * through java.util.zip.GZIPOutputStream, whose writeHeader() emits the
     * fixed 10-byte header {0x1f, 0x8b, 8, 0, 0,0,0,0, 0, 0} on the Java 6/8
     * runtimes of that era: FLG 0, MTIME 0, XFL 0, OS 0. An all-zero gz_header
     * reproduces zlib's default header (FLG 0, MTIME 0, XFL from the level)
     * with only the OS byte changed. Readers ignore the byte, so saves carrying
     * 0x0a/0x03 still load; the unchanged-NBT skip in level_archive_flush
     * compares uncompressed bytes, so they are not rewritten just for this. */
    gz_header header;
    if(strat == STRAT_GZIP) {
        memset(&header, 0, sizeof(header));
        header.os = 0;
        if(deflateSetHeader(&stream, &header) != Z_OK)
        {
            errno = NBT_EZ;
            (void)deflateEnd(&stream);
            return BUFFER_INIT;
        }
    }

    assert(stream.avail_in == len); /* I'm not sure if zlib will clobber this */

    for(;;) {
        if(buffer_reserve(&ret, ret.len + CHUNK_SIZE))
        {
            errno = NBT_EMEM;
            goto compression_error;
        }

        stream.next_out  = ret.data + ret.len;
        stream.avail_out = CHUNK_SIZE;

        int zlib_ret = deflate(&stream, Z_FINISH);
        size_t produced = CHUNK_SIZE - stream.avail_out;
        ret.len += CHUNK_SIZE - stream.avail_out;

        if(zlib_ret == Z_STREAM_END)
            break;
        if(zlib_ret != Z_OK || produced == 0)
            goto compression_error;
    }

    (void)deflateEnd(&stream);
    return ret;

compression_error:
    if(errno == NBT_OK)
        errno = NBT_EZ;

    (void)deflateEnd(&stream);
    buffer_free(&ret);
    return BUFFER_INIT;
}

/*
 * Reads in zlib-compressed data, and returns a buffer with the decompressed
 * data within. Returns a NULL buffer on failure, and sets errno appropriately.
 */
static struct buffer __decompress(const void* mem, size_t len)
{
    struct buffer ret = BUFFER_INIT;

    errno = NBT_OK;

    if(mem == NULL || len == 0) return (errno = NBT_ERR), BUFFER_INIT;
    if(len > NBT_MAX_COMPRESSED_BYTES || len > UINT_MAX)
        return (errno = NBT_EREFUSED), BUFFER_INIT;

    z_stream stream = {
        .zalloc   = Z_NULL,
        .zfree    = Z_NULL,
        .opaque   = Z_NULL,
        .next_in  = (void*)mem,
        .avail_in = (uInt)len
    };

    /* "Add 32 to windowBits to enable zlib and gzip decoding with automatic
     * header detection" */
    int init_status = inflateInit2(&stream, 15 + 32);
    if(init_status != Z_OK)
    {
        /* Initialization has not inspected content; every failure is operational. */
        errno = init_status == Z_MEM_ERROR ? NBT_EMEM : NBT_EIO;
        return BUFFER_INIT;
    }

    for(;;) {
        if(ret.len >= NBT_MAX_DECOMPRESSED_BYTES)
        {
            errno = NBT_EREFUSED;
            goto decompression_error;
        }

        size_t request = CHUNK_SIZE;
        if(request > NBT_MAX_DECOMPRESSED_BYTES - ret.len)
            request = NBT_MAX_DECOMPRESSED_BYTES - ret.len;

        if(buffer_reserve(&ret, ret.len + request))
        {
            errno = NBT_EMEM;
            goto decompression_error;
        }

        stream.avail_out = (uInt)request;
        stream.next_out  = (unsigned char*)ret.data + ret.len;

        int zlib_ret = inflate(&stream, Z_NO_FLUSH);
        size_t produced = request - stream.avail_out;
        ret.len += produced;

        switch(zlib_ret)
        {
        case Z_MEM_ERROR:
            errno = NBT_EMEM;
            goto decompression_error;

        case Z_DATA_ERROR: case Z_NEED_DICT:
            goto decompression_error;

        case Z_STREAM_END:
            (void)inflateEnd(&stream);
            return ret;

        case Z_OK:
            if(produced == 0 && stream.avail_in == 0)
                goto decompression_error;
            break;

        case Z_BUF_ERROR: /* Incomplete compressed content at physical EOF. */
            goto decompression_error;

        default: /* API/internal failures are not evidence about file content. */
            errno = NBT_EIO;
            goto decompression_error;
        }
    }

decompression_error:
    if(errno == NBT_OK)
        errno = NBT_EZ;

    (void)inflateEnd(&stream);

    buffer_free(&ret);
    return BUFFER_INIT;
}

/*
 * No incremental parsing goes on. We just dump the whole compressed file into
 * memory then pass the job off to nbt_parse_chunk.
 */
nbt_node* nbt_parse_file(FILE* fp)
{
    errno = NBT_OK;

    struct buffer compressed = read_file(fp);

    if(compressed.data == NULL)
        return NULL;

    nbt_node* ret = nbt_parse_compressed(compressed.data, compressed.len);

    buffer_free(&compressed);
    return ret;
}

nbt_node* nbt_parse_path(const char* filename)
{
    FILE* fp = fopen(filename, "rb");

    if(fp == NULL)
    {
        errno = errno == ENOENT ? NBT_ENOENT : NBT_EIO;
        return NULL;
    }

    nbt_node* r = nbt_parse_file(fp);
    int status = errno;
    if(fclose(fp)) {
        nbt_free(r);
        r = NULL;
        status = NBT_EIO;
    }
    errno = status;
    return r;
}

nbt_node* nbt_parse_compressed(const void* chunk_start, size_t length)
{
    struct buffer decompressed = __decompress(chunk_start, length);

    if(decompressed.data == NULL)
        return NULL;

    nbt_node* ret = nbt_parse(decompressed.data, decompressed.len);

    buffer_free(&decompressed);
    return ret;
}

/*
 * Once again, all we're doing is handing the actual compression off to
 * nbt_dump_compressed, then dumping it into the file.
 */
nbt_status nbt_dump_file(const nbt_node* tree, FILE* fp, nbt_compression_strategy strat)
{
    struct buffer compressed = nbt_dump_compressed(tree, strat);

    if(compressed.data == NULL)
        return (nbt_status)errno;

    nbt_status ret = write_file(fp, compressed.data, compressed.len);

    buffer_free(&compressed);
    return ret;
}

struct buffer nbt_dump_compressed_hint(const nbt_node* tree,
                                       nbt_compression_strategy strat,
                                       size_t size_hint)
{
    struct buffer uncompressed = nbt_dump_binary_hint(tree, size_hint);

    if(uncompressed.data == NULL)
        return BUFFER_INIT;

    struct buffer compressed = __compress(uncompressed.data, uncompressed.len, strat);

    buffer_free(&uncompressed);
    return compressed;
}

struct buffer nbt_compress_binary(const void* bytes, size_t length,
                                  nbt_compression_strategy strat)
{
    return __compress(bytes, length, strat);
}

struct buffer nbt_dump_compressed(const nbt_node* tree, nbt_compression_strategy strat)
{
    return nbt_dump_compressed_hint(tree, strat, 0);
}
