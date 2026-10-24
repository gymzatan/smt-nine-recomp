#ifndef NV2A_COLOR_LOGIC_H
#define NV2A_COLOR_LOGIC_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static inline uint32_t color_logic(unsigned operation,uint32_t source,uint32_t destination)
{
    switch(operation) {
    case 0:return 0;case 1:return source&destination;case 2:return source&~destination;
    case 3:return source;case 4:return ~source&destination;case 5:return destination;
    case 6:return source^destination;case 7:return source|destination;case 8:return ~(source|destination);
    case 9:return ~(source^destination);case 10:return ~destination;case 11:return source|~destination;
    case 12:return ~source;case 13:return ~source|destination;case 14:return ~(source&destination);
    case 15:return UINT32_MAX;
    }
    fprintf(stderr,"[STOP] invalid colour logic operation %u\n",operation);exit(5);
}

#endif
