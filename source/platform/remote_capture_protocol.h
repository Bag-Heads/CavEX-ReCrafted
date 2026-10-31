#ifndef CW_REMOTE_CAPTURE_PROTOCOL_H
#define CW_REMOTE_CAPTURE_PROTOCOL_H
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
/* Fixed grammar; no remote path, input injection, or unauthenticated capture. */
/* Trusted isolated development LAN only: reusable plaintext bearer token.
 * Client-compatible provisioned tokens: 1..128 bytes, printable non-space ASCII
 * (0x21..0x7E). Client trims only token-file trailing CR/LF and validates before
 * connecting. The unchanged server accepts a wider non-NUL, non-whitespace byte
 * set; the client is deliberately stricter. This is a client provisioning domain,
 * not a change to server parsing or the wire protocol.
 * Nonce echo is response correlation, not request replay protection or server
 * authentication; an observer can reuse the token. Hardening is out of scope.
 * Receiver sends one '+' after the full response header, then one after each
 * 1024-byte payload piece and the final short piece. Physical send prefixes do
 * not create credit boundaries. */
static inline bool remote_capture_auth(const char* line, const char* secret,
                                      char nonce[33]) {
    char token[129], extra;
    if(!secret || !secret[0] || strlen(secret) > 128
       || sscanf(line, "CWSHOT1 %128s %32s %c", token, nonce, &extra) != 2)
        return false;
    if(strlen(nonce) != 32) return false;
    for(unsigned i=0;i<32;i++)
        if(!((nonce[i]>='0' && nonce[i]<='9') || (nonce[i]>='a' && nonce[i]<='f')))
            return false;
    size_t n=strlen(secret), m=strlen(token);
    unsigned diff=(unsigned)(n != m);
    for(size_t i=0;i<n;i++) diff |= (unsigned char)secret[i] ^ (i<m ? (unsigned char)token[i] : 0);
    return diff == 0;
}
#endif
