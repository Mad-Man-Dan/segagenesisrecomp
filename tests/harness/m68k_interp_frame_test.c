#include "m68k_interp.h"

#include <stdio.h>
#include <string.h>

M68KState g_cpu;
uint8_t g_rom[0x400000];
uint8_t g_ram[0x010000];
uint64_t g_native_insn_count;
uint32_t g_cycle_accumulator;
uint32_t g_vblank_threshold = 0xFFFFFFFFu;
uint32_t g_audio_cycle_counter;
static int s_rte_pending;
int *g_rte_pending_ptr = &s_rte_pending;

void glue_check_vblank(void) {}

uint8_t m68k_read8(uint32_t addr)
{
    addr &= 0xFFFFFFu;
    if (addr >= RAM_BASE) return g_ram[addr & 0xFFFFu];
    return addr < ROM_SIZE ? g_rom[addr] : 0xFFu;
}

uint16_t m68k_read16(uint32_t addr)
{
    return (uint16_t)(((uint16_t)m68k_read8(addr) << 8) |
                      m68k_read8(addr + 1u));
}

uint32_t m68k_read32(uint32_t addr)
{
    return ((uint32_t)m68k_read16(addr) << 16) | m68k_read16(addr + 2u);
}

void m68k_write8(uint32_t addr, uint8_t value)
{
    addr &= 0xFFFFFFu;
    if (addr >= RAM_BASE) g_ram[addr & 0xFFFFu] = value;
}

void m68k_write16(uint32_t addr, uint16_t value)
{
    m68k_write8(addr, (uint8_t)(value >> 8));
    m68k_write8(addr + 1u, (uint8_t)value);
}

void m68k_write32(uint32_t addr, uint32_t value)
{
    m68k_write16(addr, (uint16_t)(value >> 16));
    m68k_write16(addr + 2u, (uint16_t)value);
}

static int s_failures;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); \
        s_failures++; \
    } \
} while (0)

static void reset_cpu(uint32_t sp, uint32_t ret)
{
    memset(&g_cpu, 0, sizeof(g_cpu));
    memset(g_ram, 0, sizeof(g_ram));
    s_rte_pending = 0;
    g_native_insn_count = 0;
    g_cycle_accumulator = 0;
    g_audio_cycle_counter = 0;
    g_cpu.A[7] = sp;
    m68k_write32(sp, ret);
}

static void put_words(uint32_t addr, const uint16_t *words, size_t count)
{
    for (size_t i = 0; i < count; i++)
        m68k_write16(addr + (uint32_t)i * 2u, words[i]);
}

static void put_rom_words(uint32_t addr, const uint16_t *words, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        g_rom[addr + i * 2u] = (uint8_t)(words[i] >> 8);
        g_rom[addr + i * 2u + 1u] = (uint8_t)words[i];
    }
}

static void test_direct_outer_return(void)
{
    const uint32_t entry = 0xFF0100u, sp = 0xFFF000u, ret = 0x001234u;
    const uint16_t code[] = { 0x4E75u };
    uint32_t exit_pc = 0;
    reset_cpu(sp, ret);
    put_words(entry, code, 1);
    CHECK(m68k_interp_run_framed(entry, &exit_pc) == M68KI_OK);
    CHECK(exit_pc == ret);
    CHECK(g_cpu.A[7] == sp);
}

static void test_nested_bsr(void)
{
    const uint32_t entry = 0xFF0200u, sp = 0xFFF000u, ret = 0x001236u;
    /* bsr.s sub; moveq #1,d0; rts; sub: moveq #2,d1; rts */
    const uint16_t code[] = { 0x6104u, 0x7001u, 0x4E75u, 0x7202u, 0x4E75u };
    uint32_t exit_pc = 0;
    reset_cpu(sp, ret);
    put_words(entry, code, sizeof(code) / sizeof(code[0]));
    CHECK(m68k_interp_run_framed(entry, &exit_pc) == M68KI_OK);
    CHECK(exit_pc == ret);
    CHECK(g_cpu.D[0] == 1u);
    CHECK(g_cpu.D[1] == 2u);
    CHECK(g_cpu.A[7] == sp);
}

static void test_computed_jump(void)
{
    const uint32_t entry = 0xFF0280u, body = 0xFF02A0u;
    const uint32_t sp = 0xFFF000u, ret = 0x001237u;
    const uint16_t entry_code[] = {
        0x207Cu, 0x00FFu, 0x02A0u,  /* movea.l #body,a0 */
        0x4ED0u                     /* jmp (a0) */
    };
    const uint16_t body_code[] = { 0x7621u, 0x4E75u }; /* moveq #$21,d3; rts */
    uint32_t exit_pc = 0;
    reset_cpu(sp, ret);
    put_words(entry, entry_code, sizeof(entry_code) / sizeof(entry_code[0]));
    put_words(body, body_code, sizeof(body_code) / sizeof(body_code[0]));
    CHECK(m68k_interp_run_framed(entry, &exit_pc) == M68KI_OK);
    CHECK(exit_pc == ret);
    CHECK(g_cpu.D[3] == 0x21u);
    CHECK(g_cpu.A[7] == sp);
}

static void test_synthetic_frame_and_movem(void)
{
    const uint32_t entry = 0xFF0300u, body = 0xFF0340u;
    const uint32_t continuation = 0xFF0360u, sp = 0xFFF000u, ret = 0x001238u;
    const uint16_t entry_code[] = {
        0x48E7u, 0x020Cu,             /* movem.l d6/a4-a5,-(a7) */
        0x4879u, 0x00FFu, 0x0360u,   /* pea continuation */
        0x4EF9u, 0x00FFu, 0x0340u    /* jmp body */
    };
    const uint16_t body_code[] = {
        0x7C2Au,                     /* moveq #$2a,d6 */
        0x287Cu, 0x1111u, 0x1111u,  /* movea.l #$11111111,a4 */
        0x2A7Cu, 0x2222u, 0x2222u,  /* movea.l #$22222222,a5 */
        0x4E75u
    };
    const uint16_t continuation_code[] = {
        0x4CDFu, 0x3040u,            /* movem.l (a7)+,d6/a4-a5 */
        0x4E75u
    };
    uint32_t exit_pc = 0;
    reset_cpu(sp, ret);
    g_cpu.D[6] = 0xD6D6D6D6u;
    g_cpu.A[4] = 0xA4A4A4A4u;
    g_cpu.A[5] = 0xA5A5A5A5u;
    put_words(entry, entry_code, sizeof(entry_code) / sizeof(entry_code[0]));
    put_words(body, body_code, sizeof(body_code) / sizeof(body_code[0]));
    put_words(continuation, continuation_code,
              sizeof(continuation_code) / sizeof(continuation_code[0]));
    CHECK(m68k_interp_run_ram_handler(entry, &exit_pc) == M68KI_OK);
    CHECK(exit_pc == ret);
    CHECK(g_cpu.A[7] == sp);
    CHECK(g_cpu.D[6] == 0xD6D6D6D6u);
    CHECK(g_cpu.A[4] == 0xA4A4A4A4u);
    CHECK(g_cpu.A[5] == 0xA5A5A5A5u);

}

static void test_self_modifying_ram(void)
{
    const uint32_t entry = 0xFF0380u, sp = 0xFFF000u, ret = 0x001239u;
    const uint16_t code[] = { 0x7001u, 0x4E75u }; /* moveq #1,d0; rts */
    uint32_t exit_pc = 0;
    reset_cpu(sp, ret);
    put_words(entry, code, sizeof(code) / sizeof(code[0]));
    CHECK(m68k_interp_run_ram_handler(entry, &exit_pc) == M68KI_OK);
    CHECK(g_cpu.D[0] == 1u);

    /* The next run must decode the modified live-RAM instruction. */
    m68k_write16(entry, 0x7002u); /* moveq #2,d0 */
    g_cpu.D[0] = 0;
    g_cpu.A[7] = sp;
    m68k_write32(sp, ret);
    exit_pc = 0;
    CHECK(m68k_interp_run_ram_handler(entry, &exit_pc) == M68KI_OK);
    CHECK(exit_pc == ret);
    CHECK(g_cpu.D[0] == 2u);
    CHECK(g_cpu.A[7] == sp);
}

static void test_rom_handler_synthetic_frame(void)
{
    const uint32_t entry = 0x001000u, body = 0x001020u;
    const uint32_t continuation = 0x001040u, sp = 0xFFF000u;
    const uint16_t entry_code[] = {
        0x4879u, 0x0000u, 0x1040u,  /* pea continuation */
        0x4EF9u, 0x0000u, 0x1020u   /* jmp body */
    };
    const uint16_t body_code[] = { 0x7844u, 0x4E75u }; /* moveq #$44,d4; rts */
    const uint16_t continuation_code[] = { 0x4E73u };   /* rte */
    reset_cpu(sp, 0);
    put_rom_words(entry, entry_code, sizeof(entry_code) / sizeof(entry_code[0]));
    put_rom_words(body, body_code, sizeof(body_code) / sizeof(body_code[0]));
    put_rom_words(continuation, continuation_code, 1);
    CHECK(m68k_interp_run_handler(entry) == M68KI_OK);
    CHECK(g_cpu.D[4] == 0x44u);
    CHECK(g_cpu.A[7] == sp);
}

static void test_bad_frame(void)
{
    const uint32_t entry = 0xFF0400u, sp = 0xFFF000u, ret = 0x00123Au;
    const uint16_t code[] = { 0x205Fu, 0x4E75u }; /* movea.l (a7)+,a0; rts */
    uint32_t exit_pc = 0;
    reset_cpu(sp, ret);
    put_words(entry, code, sizeof(code) / sizeof(code[0]));
    CHECK(m68k_interp_run_framed(entry, &exit_pc) == M68KI_HALT_BADFRAME);
    CHECK(g_m68ki_bad_pc == entry + 2u);
}

static void test_rte_unwinds_to_entry_boundary(void)
{
    const uint32_t entry = 0xFF0500u, body = 0xFF0520u;
    const uint32_t continuation = 0xFF0540u, sp = 0xFFF000u, ret = 0x00123Cu;
    const uint16_t entry_code[] = {
        0x4879u, 0x00FFu, 0x0540u,   /* synthetic frame */
        0x4EF9u, 0x00FFu, 0x0520u
    };
    const uint16_t body_code[] = { 0x4E73u };
    uint32_t exit_pc = 0;
    reset_cpu(sp, ret);
    put_words(entry, entry_code, sizeof(entry_code) / sizeof(entry_code[0]));
    put_words(body, body_code, 1);
    CHECK(m68k_interp_run_ram_handler(entry, &exit_pc) == M68KI_OK);
    CHECK(s_rte_pending == 1);
    CHECK(g_cpu.A[7] == sp);
    CHECK(exit_pc == 0u);
}

int main(void)
{
    test_direct_outer_return();
    test_nested_bsr();
    test_computed_jump();
    test_synthetic_frame_and_movem();
    test_self_modifying_ram();
    test_rom_handler_synthetic_frame();
    test_bad_frame();
    test_rte_unwinds_to_entry_boundary();
    if (s_failures) {
        fprintf(stderr, "m68k_interp_frame_test: %d failure(s)\n", s_failures);
        return 1;
    }
    puts("m68k_interp_frame_test: PASS");
    return 0;
}
