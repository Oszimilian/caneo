// Test target for the ELF reader / XCP simulator: build with
//   gcc -g -O0 -no-pie -o tests/data/xcp_target.elf tests/data/xcp_target.c
#include <stdbool.h>
#include <stdint.h>

typedef enum { MODE_OFF = 0, MODE_RUN = 1, MODE_FAULT = -1 } Mode;

typedef struct {
    float kp, ki, kd;
    int16_t limit;
} Pid;

struct Flags {
    uint8_t enable : 1;
    uint8_t dir    : 1;
    uint8_t level  : 3;
    int8_t  trim   : 3;
};

struct Ctrl {
    Pid      pid;
    Mode     mode;
    struct Flags flags;
    union { uint32_t raw; float f; } u;
    uint16_t table[4][3];
    struct { int a; int b; };   // anonymous
};

volatile float    sine        = 0.0f;
volatile uint32_t counter     = 0;
volatile double   setpoint    = 1.5;
int8_t            offset      = -3;
bool              active      = true;
struct Ctrl       ctrl        = { { 1.0f, 0.1f, 0.0f, 100 }, MODE_RUN, { 1, 0, 5, -2 }, { 0 }, { { 0 } }, { 1, 2 } };
const char*       name_ptr    = "x";
uint8_t           big_buffer[1000];

static uint16_t file_static = 7;

int step(void)
{
    static int calls = 0;
    ++calls;
    counter += 1;
    return calls + file_static + big_buffer[0];
}

int main(void) { return step(); }
