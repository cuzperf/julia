/* equalhash 哈希表头文件。
 * 声明了一个使用等式语义（而非恒等语义）的哈希表原型，
 * 用于在 Julia 编译器中按值（而非引用）进行哈希查找。 */
#ifndef JL_EQUALHASH_H
#define JL_EQUALHASH_H

#include "htable.h"

#ifdef __cplusplus
extern "C" {
#endif

HTPROT_R(equalhash)

#ifdef __cplusplus
}
#endif

#endif
