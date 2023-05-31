#ifndef KRX_KXIPC_ENCODING_H
#define KRX_KXIPC_ENCODING_H

#include <stdint.h>

#define _C_ID '@'
#define _C_CLASS '#'
#define _C_SEL ':'
#define _C_CHR 'c'
#define _C_UCHR 'C'
#define _C_SHT 's'
#define _C_USHT 'S'
#define _C_INT 'i'
#define _C_UINT 'I'
#define _C_LNG 'l'
#define _C_ULNG 'L'
#define _C_LNG_LNG 'q'
#define _C_ULNG_LNG 'Q'
#define _C_FLT 'f'
#define _C_DBL 'd'
#define _C_BFLD 'b'
#define _C_BOOL 'B'
#define _C_VOID 'v'
#define _C_UNDEF '?'
#define _C_PTR '^'
#define _C_CHARPTR '*'
#define _C_ATOM '%'
#define _C_ARY_B '['
#define _C_ARY_E ']'
#define _C_UNION_B '('
#define _C_UNION_E ')'
#define _C_STRUCT_B '{'
#define _C_STRUCT_E '}'
#define _C_VECTOR '!'
#define _C_CONST 'r'

#define ROUNDUP(addr, align) (((addr) + align - 1) & ~(align - 1))
#define ROUNDDOWN(addr, align) ((((uintptr_t)addr)) & ~(align - 1))

size_t objc_sizeof_type(const char *type);
const char *OFGetSizeAndAlignment(const char *typePtr, size_t *sizep,
    size_t *alignp);

#endif /* KRX_KXIPC_ENCODING_H */
