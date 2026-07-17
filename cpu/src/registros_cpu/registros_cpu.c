#include "cpu/cpu.h"
#include "registros_cpu.h"

#include <string.h>

uint8_t get_registro(struct t_cpu* cpu, char* nombre) {
    if (strcmp(nombre, "AX") == 0) return cpu->registros.ax;
    if (strcmp(nombre, "BX") == 0) return cpu->registros.bx;
    if (strcmp(nombre, "CX") == 0) return cpu->registros.cx;
    if (strcmp(nombre, "DX") == 0) return cpu->registros.dx;
    return 0;
}

void set_registro(struct t_cpu* cpu, char* nombre, uint8_t valor) {
    if (strcmp(nombre, "AX") == 0) { cpu->registros.ax = valor; return; }
    if (strcmp(nombre, "BX") == 0) { cpu->registros.bx = valor; return; }
    if (strcmp(nombre, "CX") == 0) { cpu->registros.cx = valor; return; }
    if (strcmp(nombre, "DX") == 0) { cpu->registros.dx = valor; return; }
}

uint32_t get_registro_32(struct t_cpu* cpu, char* nombre) {
    if (strcmp(nombre, "EAX") == 0) return cpu->registros.eax;
    if (strcmp(nombre, "EBX") == 0) return cpu->registros.ebx;
    if (strcmp(nombre, "ECX") == 0) return cpu->registros.ecx;
    if (strcmp(nombre, "EDX") == 0) return cpu->registros.edx;
    if (strcmp(nombre, "SI") == 0) return cpu->registros.si;
    if (strcmp(nombre, "DI") == 0) return cpu->registros.di;
    if (strcmp(nombre, "PC") == 0) return cpu->registros.pc;
    if (strcmp(nombre, "AX") == 0) return (uint32_t) cpu->registros.ax;
    if (strcmp(nombre, "BX") == 0) return (uint32_t) cpu->registros.bx;
    if (strcmp(nombre, "CX") == 0) return (uint32_t) cpu->registros.cx;
    if (strcmp(nombre, "DX") == 0) return (uint32_t) cpu->registros.dx;
    return 0;
}

uint32_t tamanio_registro(char* nombre) {
    if (strcmp(nombre, "AX") == 0 || strcmp(nombre, "BX") == 0 ||
        strcmp(nombre, "CX") == 0 || strcmp(nombre, "DX") == 0) {
        return 1;
    }
    return 4;
}

void set_registro_32(struct t_cpu* cpu, char* nombre, uint32_t valor) {
    if (strcmp(nombre, "EAX") == 0) { cpu->registros.eax = valor; return; }
    if (strcmp(nombre, "EBX") == 0) { cpu->registros.ebx = valor; return; }
    if (strcmp(nombre, "ECX") == 0) { cpu->registros.ecx = valor; return; }
    if (strcmp(nombre, "EDX") == 0) { cpu->registros.edx = valor; return; }
    if (strcmp(nombre, "SI") == 0) { cpu->registros.si = valor; return; }
    if (strcmp(nombre, "DI") == 0) { cpu->registros.di = valor; return; }
    if (strcmp(nombre, "PC") == 0) { cpu->registros.pc = valor; return; }
    if (strcmp(nombre, "AX") == 0) { cpu->registros.ax = (uint8_t) valor; return; }
    if (strcmp(nombre, "BX") == 0) { cpu->registros.bx = (uint8_t) valor; return; }
    if (strcmp(nombre, "CX") == 0) { cpu->registros.cx = (uint8_t) valor; return; }
    if (strcmp(nombre, "DX") == 0) { cpu->registros.dx = (uint8_t) valor; return; }
}
