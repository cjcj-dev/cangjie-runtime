#include <stdint.h>
void CJ_MCC_WriteRefField(void *val, void *obj, void **field)
{
    (void)obj;
    *field = val;
}
void CJ_MCC_PostWriteRefField(void *val, void *obj, void **field, uintptr_t prev)
{
    (void)val;
    (void)obj;
    (void)field;
    (void)prev;
}
void MCC_WriteRefField(void *val, void *obj, void **field)
{
    CJ_MCC_WriteRefField(val, obj, field);
}
