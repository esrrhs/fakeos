package arch;
import types;

extern void outb(types.uint16_t port, types.uint8_t val);
extern types.uint8_t inb(types.uint16_t port);
extern void outw(types.uint16_t port, types.uint16_t val);
extern types.uint16_t inw(types.uint16_t port);
extern void outl(types.uint16_t port, types.uint32_t val);
extern types.uint32_t inl(types.uint16_t port);
extern void io_wait(void);
extern void cpu_halt(void);
extern void cpu_cli(void);
extern void cpu_sti(void);
