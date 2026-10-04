#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ELF_MAGIC "\x7f" "ELF"
#define ELFCLASS32 1U
#define ELFDATA2LSB 1U
#define EV_CURRENT 1U
#define ET_REL 1U
#define EM_386 3U

#define SHT_PROGBITS 1U
#define SHT_SYMTAB 2U
#define SHT_STRTAB 3U
#define SHT_RELA 4U
#define SHT_NOBITS 8U
#define SHT_REL 9U

#define SHF_WRITE 1U
#define SHF_ALLOC 2U
#define SHF_EXECINSTR 4U
#define SHF_MERGE 0x10U
#define SHF_STRINGS 0x20U
#define SUPPORTED_ALLOC_FLAGS \
    (SHF_WRITE | SHF_ALLOC | SHF_EXECINSTR | SHF_MERGE | SHF_STRINGS)
#define SHN_UNDEF 0U
#define SHN_ABS 0xFFF1U
#define SHN_COMMON 0xFFF2U

#define STB_LOCAL 0U
#define STB_GLOBAL 1U
#define STB_WEAK 2U

#define R_386_32 1U
#define R_386_PC32 2U

#define ELF32_ST_BIND(info) ((uint8_t)((info) >> 4))
#define ELF32_R_SYM(value) ((value) >> 8)
#define ELF32_R_TYPE(value) ((value) & 0xFFU)

#define MAX_INPUT_SIZE (64U * 1024U * 1024U)
#define DEFAULT_MAX_OUTPUT_SIZE (512U * 1024U)
#define DEFAULT_MAX_OBJECTS 256U
#define DEFAULT_MAX_SYMBOLS 4096U
#define DEFAULT_MAX_SECTIONS 4096U
#define DEFAULT_MAX_RELOCATIONS 32768U
#define HARD_MAX_OUTPUT_SIZE (64U * 1024U * 1024U)
#define HARD_MAX_OBJECTS 4096U
#define HARD_MAX_SYMBOLS 65536U
#define HARD_MAX_SECTIONS 65535U
#define HARD_MAX_RELOCATIONS (1024U * 1024U)
#define UNPLACED_SECTION UINT32_MAX

#pragma pack(push, 1)
typedef struct {
    unsigned char e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf32_Ehdr;

typedef struct {
    uint32_t sh_name;
    uint32_t sh_type;
    uint32_t sh_flags;
    uint32_t sh_addr;
    uint32_t sh_offset;
    uint32_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint32_t sh_addralign;
    uint32_t sh_entsize;
} Elf32_Shdr;

typedef struct {
    uint32_t st_name;
    uint32_t st_value;
    uint32_t st_size;
    uint8_t st_info;
    uint8_t st_other;
    uint16_t st_shndx;
} Elf32_Sym;

typedef struct {
    uint32_t r_offset;
    uint32_t r_info;
} Elf32_Rel;
#pragma pack(pop)

typedef struct {
    const char *name;
    uint32_t address;
    int defined;
    int weak;
} GlobalSymbol;

typedef struct {
    const char *name;
    uint8_t *data;
    size_t size;
    Elf32_Ehdr *header;
    Elf32_Shdr *sections;
    const char *section_names;
    size_t section_names_size;
    uint32_t *output_offsets;
    uint64_t relocation_count;
} ObjectFile;

typedef struct {
    uint32_t max_output_size;
    uint32_t max_objects;
    uint32_t max_symbols;
    uint32_t max_sections;
    uint32_t max_relocations;
} CapacityLimits;

static CapacityLimits limits = {
    DEFAULT_MAX_OUTPUT_SIZE,
    DEFAULT_MAX_OBJECTS,
    DEFAULT_MAX_SYMBOLS,
    DEFAULT_MAX_SECTIONS,
    DEFAULT_MAX_RELOCATIONS
};
static GlobalSymbol *global_symbols;
static size_t global_symbol_count;
static uint8_t *output_buffer;
static uint32_t output_size;
static uint64_t total_relocations;

static int range_valid(uint64_t offset, uint64_t size, uint64_t total) {
    return offset <= total && size <= total - offset;
}

static int power_of_two(uint32_t value) {
    return value != 0U && (value & (value - 1U)) == 0U;
}

static void print_usage(const char *program) {
    fprintf(stderr,
            "Usage: %s [capacity options] <output.bin> <base_addr> "
            "<obj1.o> [obj2.o ...]\n"
            "Capacity options:\n"
            "  --max-output <bytes>\n"
            "  --max-objects <count>\n"
            "  --max-symbols <count>\n"
            "  --max-sections <count-per-object>\n"
            "  --max-relocations <count>\n",
            program);
}

static int parse_capacity(const char *option, const char *text,
                          uint32_t hard_maximum, uint32_t *value_out) {
    char *end;
    unsigned long long parsed;

    errno = 0;
    parsed = strtoull(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || parsed == 0U ||
        parsed > hard_maximum) {
        fprintf(stderr, "Error: invalid %s capacity '%s' (maximum %u).\n",
                option, text, hard_maximum);
        return -1;
    }
    *value_out = (uint32_t)parsed;
    return 0;
}

static int set_capacity_option(const char *option, const char *value) {
    if (strcmp(option, "--max-output") == 0) {
        return parse_capacity(option, value, HARD_MAX_OUTPUT_SIZE,
                              &limits.max_output_size);
    }
    if (strcmp(option, "--max-objects") == 0) {
        return parse_capacity(option, value, HARD_MAX_OBJECTS,
                              &limits.max_objects);
    }
    if (strcmp(option, "--max-symbols") == 0) {
        return parse_capacity(option, value, HARD_MAX_SYMBOLS,
                              &limits.max_symbols);
    }
    if (strcmp(option, "--max-sections") == 0) {
        return parse_capacity(option, value, HARD_MAX_SECTIONS,
                              &limits.max_sections);
    }
    if (strcmp(option, "--max-relocations") == 0) {
        return parse_capacity(option, value, HARD_MAX_RELOCATIONS,
                              &limits.max_relocations);
    }
    fprintf(stderr, "Error: unknown capacity option '%s'.\n", option);
    return -1;
}

static int table_string(const char *table, size_t table_size, uint32_t offset,
                        const char **string_out) {
    if (offset >= table_size || memchr(table + offset, '\0', table_size - offset) == NULL) {
        return -1;
    }
    *string_out = table + offset;
    return 0;
}

static int add_global_symbol(const char *name, uint32_t address,
                             int defined, int weak) {
    size_t i;

    if (name == NULL || name[0] == '\0') return 0;
    for (i = 0; i < global_symbol_count; i++) {
        if (strcmp(global_symbols[i].name, name) != 0) continue;
        if (!defined) {
            if (!global_symbols[i].defined && !weak) {
                global_symbols[i].weak = 0;
            }
            return 0;
        }
        if (!global_symbols[i].defined) {
            global_symbols[i].address = address;
            global_symbols[i].defined = 1;
            global_symbols[i].weak = weak;
            return 0;
        }
        if (!global_symbols[i].weak && !weak) {
            fprintf(stderr, "Error: duplicate strong symbol '%s'.\n", name);
            return -1;
        }
        if (global_symbols[i].weak && !weak) {
            global_symbols[i].address = address;
            global_symbols[i].weak = 0;
        }
        return 0;
    }
    if (global_symbol_count >= limits.max_symbols) {
        fprintf(stderr, "Error: global symbol capacity (%u) exceeded.\n",
                limits.max_symbols);
        return -1;
    }
    global_symbols[global_symbol_count].name = name;
    global_symbols[global_symbol_count].address = address;
    global_symbols[global_symbol_count].defined = defined;
    global_symbols[global_symbol_count].weak = weak;
    global_symbol_count++;
    return 0;
}

static int find_global_symbol(const char *name, uint32_t *address_out) {
    size_t i;

    for (i = 0; i < global_symbol_count; i++) {
        if (strcmp(global_symbols[i].name, name) == 0) {
            if (!global_symbols[i].defined) {
                if (global_symbols[i].weak) {
                    *address_out = 0U;
                    return 0;
                }
                fprintf(stderr, "Error: symbol '%s' is undefined.\n", name);
                return -1;
            }
            *address_out = global_symbols[i].address;
            return 0;
        }
    }
    fprintf(stderr, "Error: symbol '%s' is missing.\n", name);
    return -1;
}

static void free_objects(ObjectFile *objects, int count) {
    int i;

    if (objects == NULL) return;
    for (i = 0; i < count; i++) {
        free(objects[i].output_offsets);
        free(objects[i].data);
    }
    free(objects);
}

static int validate_section(ObjectFile *object, uint32_t index) {
    Elf32_Shdr *section;
    const char *section_name;

    section = &object->sections[index];
    if (table_string(object->section_names, object->section_names_size,
                     section->sh_name, &section_name) < 0) {
        fprintf(stderr, "Error: %s section %u has an invalid name offset.\n",
                object->name, index);
        return -1;
    }
    if (section->sh_type != SHT_NOBITS &&
        !range_valid(section->sh_offset, section->sh_size, object->size)) {
        fprintf(stderr, "Error: %s section %u lies outside the file.\n",
                object->name, index);
        return -1;
    }
    if (section->sh_addralign != 0U && !power_of_two(section->sh_addralign)) {
        fprintf(stderr, "Error: %s section %u has invalid alignment %u.\n",
                object->name, index, section->sh_addralign);
        return -1;
    }
    if (section->sh_type == SHT_RELA) {
        fprintf(stderr, "Error: %s uses unsupported RELA relocations.\n", object->name);
        return -1;
    }
    if ((section->sh_flags & SHF_ALLOC) != 0U &&
        section->sh_type != SHT_PROGBITS && section->sh_type != SHT_NOBITS) {
        fprintf(stderr,
                "Error: %s section %u has unsupported allocatable type %u.\n",
                object->name, index, section->sh_type);
        return -1;
    }
    if ((section->sh_flags & SHF_ALLOC) != 0U &&
        (section->sh_flags & ~SUPPORTED_ALLOC_FLAGS) != 0U) {
        fprintf(stderr,
                "Error: %s section %u has unsupported allocatable flags 0x%X.\n",
                object->name, index,
                section->sh_flags & ~SUPPORTED_ALLOC_FLAGS);
        return -1;
    }
    if ((section->sh_flags & SHF_STRINGS) != 0U &&
        (section->sh_flags & SHF_MERGE) == 0U) {
        fprintf(stderr, "Error: %s section %u has SHF_STRINGS without SHF_MERGE.\n",
                object->name, index);
        return -1;
    }
    if ((section->sh_flags & SHF_MERGE) != 0U && section->sh_entsize == 0U) {
        fprintf(stderr, "Error: %s section %u has SHF_MERGE without an entry size.\n",
                object->name, index);
        return -1;
    }
    if ((section->sh_flags & SHF_ALLOC) != 0U &&
        (strcmp(section_name, ".init_array") == 0 ||
         strncmp(section_name, ".init_array.", 12U) == 0 ||
         strcmp(section_name, ".fini_array") == 0 ||
         strncmp(section_name, ".fini_array.", 12U) == 0 ||
         strcmp(section_name, ".preinit_array") == 0 ||
         strncmp(section_name, ".preinit_array.", 15U) == 0 ||
         strcmp(section_name, ".ctors") == 0 ||
         strncmp(section_name, ".ctors.", 7U) == 0 ||
         strcmp(section_name, ".dtors") == 0 ||
         strncmp(section_name, ".dtors.", 7U) == 0)) {
        fprintf(stderr, "Error: %s uses unsupported constructor/destructor section %s.\n",
                object->name, section_name);
        return -1;
    }
    if (section->sh_type == SHT_SYMTAB &&
        (section->sh_entsize != sizeof(Elf32_Sym) ||
         section->sh_size % sizeof(Elf32_Sym) != 0U)) {
        fprintf(stderr, "Error: %s has a malformed symbol table.\n", object->name);
        return -1;
    }
    if (section->sh_type == SHT_REL &&
        (section->sh_entsize != sizeof(Elf32_Rel) ||
         section->sh_size % sizeof(Elf32_Rel) != 0U)) {
        fprintf(stderr, "Error: %s has a malformed relocation table.\n", object->name);
        return -1;
    }
    return 0;
}

static int read_object(ObjectFile *object, const char *filename) {
    FILE *file;
    long file_size;
    uint64_t section_table_size;
    Elf32_Shdr *name_section;
    uint32_t i;
    object->name = filename;
    file = fopen(filename, "rb");
    if (file == NULL) {
        fprintf(stderr, "Error: cannot open '%s': %s\n", filename, strerror(errno));
        return -1;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (file_size = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0 || file_size < (long)sizeof(Elf32_Ehdr) ||
        (uint64_t)file_size > MAX_INPUT_SIZE) {
        fprintf(stderr, "Error: invalid object size for '%s'.\n", filename);
        fclose(file);
        return -1;
    }
    object->size = (size_t)file_size;
    object->data = malloc(object->size);
    if (object->data == NULL) {
        fclose(file);
        return -1;
    }
    if (fread(object->data, 1, object->size, file) != object->size) {
        fprintf(stderr, "Error: cannot read complete object '%s'.\n", filename);
        fclose(file);
        return -1;
    }
    if (fclose(file) != 0) {
        fprintf(stderr, "Error: cannot close object '%s'.\n", filename);
        return -1;
    }

    object->header = (Elf32_Ehdr *)object->data;
    if (memcmp(object->header->e_ident, ELF_MAGIC, 4U) != 0 ||
        object->header->e_ident[4] != ELFCLASS32 ||
        object->header->e_ident[5] != ELFDATA2LSB ||
        object->header->e_ident[6] != EV_CURRENT ||
        object->header->e_type != ET_REL || object->header->e_machine != EM_386 ||
        object->header->e_version != EV_CURRENT ||
        object->header->e_ehsize != sizeof(Elf32_Ehdr) ||
        object->header->e_shentsize != sizeof(Elf32_Shdr) ||
        object->header->e_shnum == 0U ||
        object->header->e_shnum > limits.max_sections ||
        object->header->e_shstrndx >= object->header->e_shnum) {
        fprintf(stderr, "Error: '%s' is not a supported 32-bit x86 relocatable ELF.\n",
                filename);
        return -1;
    }
    section_table_size = (uint64_t)object->header->e_shnum * sizeof(Elf32_Shdr);
    if (!range_valid(object->header->e_shoff, section_table_size, object->size)) {
        fprintf(stderr, "Error: '%s' has an out-of-range section table.\n", filename);
        return -1;
    }
    object->sections = (Elf32_Shdr *)(object->data + object->header->e_shoff);
    name_section = &object->sections[object->header->e_shstrndx];
    if (name_section->sh_type != SHT_STRTAB ||
        !range_valid(name_section->sh_offset, name_section->sh_size, object->size)) {
        fprintf(stderr, "Error: '%s' has an invalid section-name table.\n", filename);
        return -1;
    }
    object->section_names = (const char *)(object->data + name_section->sh_offset);
    object->section_names_size = name_section->sh_size;
    object->output_offsets = malloc((size_t)object->header->e_shnum * sizeof(uint32_t));
    if (object->output_offsets == NULL) return -1;
    object->relocation_count = 0U;
    for (i = 0; i < object->header->e_shnum; i++) {
        object->output_offsets[i] = UNPLACED_SECTION;
        if (validate_section(object, i) < 0) return -1;
        if (object->sections[i].sh_type == SHT_REL) {
            object->relocation_count += object->sections[i].sh_size /
                                        (uint32_t)sizeof(Elf32_Rel);
        }
    }
    return 0;
}

static int place_sections(ObjectFile *object, uint32_t base_address) {
    uint32_t i;
    uint32_t alignment;
    uint64_t aligned;
    uint64_t end;
    Elf32_Shdr *section;

    for (i = 0; i < object->header->e_shnum; i++) {
        section = &object->sections[i];
        if ((section->sh_type != SHT_PROGBITS && section->sh_type != SHT_NOBITS) ||
            (section->sh_flags & SHF_ALLOC) == 0U) {
            continue;
        }
        alignment = section->sh_addralign == 0U ? 1U : section->sh_addralign;
        aligned = ((uint64_t)output_size + alignment - 1U) & ~((uint64_t)alignment - 1U);
        end = aligned + section->sh_size;
        if (aligned > UINT32_MAX || end > limits.max_output_size ||
            (uint64_t)base_address + end > UINT32_MAX) {
            fprintf(stderr,
                    "Error: allocatable sections exceed the flat-output capacity (%u).\n",
                    limits.max_output_size);
            return -1;
        }
        object->output_offsets[i] = (uint32_t)aligned;
        if (section->sh_type == SHT_PROGBITS && section->sh_size != 0U) {
            memcpy(output_buffer + aligned, object->data + section->sh_offset,
                   section->sh_size);
        } else if (section->sh_size != 0U) {
            memset(output_buffer + aligned, 0, section->sh_size);
        }
        output_size = (uint32_t)end;
    }
    return 0;
}

static int symbol_table_parts(ObjectFile *object, Elf32_Shdr *symbol_section,
                              Elf32_Sym **symbols_out, uint32_t *count_out,
                              const char **strings_out, size_t *string_size_out) {
    Elf32_Shdr *string_section;

    if (symbol_section->sh_type != SHT_SYMTAB ||
        symbol_section->sh_link >= object->header->e_shnum) return -1;
    string_section = &object->sections[symbol_section->sh_link];
    if (string_section->sh_type != SHT_STRTAB) return -1;
    *symbols_out = (Elf32_Sym *)(object->data + symbol_section->sh_offset);
    *count_out = symbol_section->sh_size / (uint32_t)sizeof(Elf32_Sym);
    *strings_out = (const char *)(object->data + string_section->sh_offset);
    *string_size_out = string_section->sh_size;
    return 0;
}

static int symbol_address(ObjectFile *object, const Elf32_Sym *symbol,
                          const char *name, uint32_t base_address,
                          uint32_t *address_out) {
    uint64_t address;

    if (symbol->st_shndx == SHN_UNDEF) return find_global_symbol(name, address_out);
    if (symbol->st_shndx == SHN_ABS) {
        *address_out = symbol->st_value;
        return 0;
    }
    if (symbol->st_shndx == SHN_COMMON) {
        fprintf(stderr, "Error: common symbol '%s' is unsupported.\n", name);
        return -1;
    }
    if (symbol->st_shndx >= object->header->e_shnum ||
        object->output_offsets[symbol->st_shndx] == UNPLACED_SECTION ||
        symbol->st_value > object->sections[symbol->st_shndx].sh_size) {
        fprintf(stderr, "Error: symbol '%s' refers to an invalid output section.\n", name);
        return -1;
    }
    address = (uint64_t)base_address + object->output_offsets[symbol->st_shndx] +
              symbol->st_value;
    if (address > UINT32_MAX) return -1;
    *address_out = (uint32_t)address;
    return 0;
}

static int collect_symbols(ObjectFile *object, uint32_t base_address) {
    uint32_t i;
    uint32_t s;
    uint32_t count;
    uint32_t address;
    Elf32_Shdr *section;
    Elf32_Sym *symbols;
    const char *strings;
    const char *name;
    size_t strings_size;
    uint8_t binding;

    for (i = 0; i < object->header->e_shnum; i++) {
        section = &object->sections[i];
        if (section->sh_type != SHT_SYMTAB) continue;
        if (symbol_table_parts(object, section, &symbols, &count,
                               &strings, &strings_size) < 0) {
            fprintf(stderr, "Error: %s has invalid symbol-table links.\n", object->name);
            return -1;
        }
        for (s = 0; s < count; s++) {
            if (table_string(strings, strings_size, symbols[s].st_name, &name) < 0) {
                fprintf(stderr, "Error: %s has an invalid symbol name.\n", object->name);
                return -1;
            }
            binding = ELF32_ST_BIND(symbols[s].st_info);
            if (binding == STB_LOCAL || name[0] == '\0') continue;
            if (binding != STB_GLOBAL && binding != STB_WEAK) {
                fprintf(stderr, "Error: %s uses unsupported symbol binding %u.\n",
                        object->name, binding);
                return -1;
            }
            if (symbols[s].st_shndx == SHN_UNDEF) {
                if (add_global_symbol(name, 0U, 0, binding == STB_WEAK) < 0) return -1;
            } else {
                if (symbol_address(object, &symbols[s], name, base_address, &address) < 0 ||
                    add_global_symbol(name, address, 1, binding == STB_WEAK) < 0) {
                    return -1;
                }
            }
        }
    }
    return 0;
}

static int ensure_symbols_resolved(void) {
    size_t i;

    for (i = 0; i < global_symbol_count; i++) {
        if (!global_symbols[i].defined && !global_symbols[i].weak) {
            fprintf(stderr, "Error: symbol '%s' remains undefined.\n",
                    global_symbols[i].name);
            return -1;
        }
    }
    return 0;
}

static int apply_relocations(ObjectFile *object, uint32_t base_address) {
    uint32_t i;
    uint32_t r;
    uint32_t relocation_count;
    uint32_t symbol_count;
    uint32_t symbol_index;
    uint32_t type;
    uint32_t target_index;
    uint32_t target_offset;
    uint32_t patch_offset;
    uint32_t symbol_value;
    uint32_t place;
    uint32_t patched;
    int32_t addend;
    Elf32_Shdr *relocation_section;
    Elf32_Shdr *target_section;
    Elf32_Shdr *symbol_section;
    Elf32_Rel *relocations;
    Elf32_Sym *symbols;
    const char *strings;
    const char *name;
    size_t strings_size;

    for (i = 0; i < object->header->e_shnum; i++) {
        relocation_section = &object->sections[i];
        if (relocation_section->sh_type != SHT_REL) continue;
        target_index = relocation_section->sh_info;
        if (target_index >= object->header->e_shnum ||
            relocation_section->sh_link >= object->header->e_shnum ||
            object->output_offsets[target_index] == UNPLACED_SECTION) {
            fprintf(stderr, "Error: %s relocation targets an invalid section.\n", object->name);
            return -1;
        }
        target_section = &object->sections[target_index];
        target_offset = object->output_offsets[target_index];
        symbol_section = &object->sections[relocation_section->sh_link];
        if (symbol_table_parts(object, symbol_section, &symbols, &symbol_count,
                               &strings, &strings_size) < 0) {
            fprintf(stderr, "Error: %s relocation has an invalid symbol table.\n", object->name);
            return -1;
        }
        relocations = (Elf32_Rel *)(object->data + relocation_section->sh_offset);
        relocation_count = relocation_section->sh_size / (uint32_t)sizeof(Elf32_Rel);
        for (r = 0; r < relocation_count; r++) {
            symbol_index = ELF32_R_SYM(relocations[r].r_info);
            type = ELF32_R_TYPE(relocations[r].r_info);
            if (symbol_index >= symbol_count ||
                !range_valid(relocations[r].r_offset, sizeof(uint32_t), target_section->sh_size)) {
                fprintf(stderr, "Error: %s relocation %u is out of range.\n", object->name, r);
                return -1;
            }
            if (type != R_386_32 && type != R_386_PC32) {
                fprintf(stderr, "Error: unsupported relocation type %u in %s.\n",
                        type, object->name);
                return -1;
            }
            if (table_string(strings, strings_size, symbols[symbol_index].st_name, &name) < 0 ||
                symbol_address(object, &symbols[symbol_index], name, base_address,
                               &symbol_value) < 0) {
                return -1;
            }
            patch_offset = target_offset + relocations[r].r_offset;
            if (!range_valid(patch_offset, sizeof(uint32_t), output_size)) return -1;
            memcpy(&addend, output_buffer + patch_offset, sizeof(addend));
            place = base_address + patch_offset;
            patched = type == R_386_32 ? symbol_value + (uint32_t)addend :
                      symbol_value + (uint32_t)addend - place;
            memcpy(output_buffer + patch_offset, &patched, sizeof(patched));
        }
    }
    return 0;
}

static int write_output(const char *path) {
    FILE *file;
    int result;

    file = fopen(path, "wb");
    if (file == NULL) {
        fprintf(stderr, "Error: cannot create '%s': %s\n", path, strerror(errno));
        return -1;
    }
    result = 0;
    if (fwrite(output_buffer, 1, output_size, file) != output_size) result = -1;
    if (fflush(file) != 0) result = -1;
    if (fclose(file) != 0) result = -1;
    if (result < 0) {
        fprintf(stderr, "Error: cannot write complete output '%s'.\n", path);
        remove(path);
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *output_path;
    char *end;
    unsigned long parsed_base;
    uint32_t base_address;
    ObjectFile *objects;
    int argument_index;
    int object_argument_index;
    int object_count;
    int i;
    int result;

    argument_index = 1;
    while (argument_index < argc && argv[argument_index][0] == '-' &&
           argv[argument_index][1] == '-') {
        if (argument_index + 1 >= argc ||
            set_capacity_option(argv[argument_index],
                                argv[argument_index + 1]) < 0) {
            print_usage(argv[0]);
            return 1;
        }
        argument_index += 2;
    }
    if (argc - argument_index < 3) {
        print_usage(argv[0]);
        return 1;
    }
    output_path = argv[argument_index++];
    errno = 0;
    parsed_base = strtoul(argv[argument_index], &end, 0);
    if (errno != 0 || end == argv[argument_index] || *end != '\0' ||
        parsed_base > UINT32_MAX) {
        fprintf(stderr, "Error: invalid base address '%s'.\n",
                argv[argument_index]);
        return 1;
    }
    argument_index++;
    base_address = (uint32_t)parsed_base;
    object_argument_index = argument_index;
    object_count = argc - object_argument_index;
    if ((uint32_t)object_count > limits.max_objects) {
        fprintf(stderr, "Error: object capacity (%u) exceeded.\n",
                limits.max_objects);
        return 1;
    }
    global_symbols = calloc(limits.max_symbols, sizeof(*global_symbols));
    output_buffer = calloc(limits.max_output_size, 1U);
    objects = calloc((size_t)object_count, sizeof(*objects));
    if (global_symbols == NULL || output_buffer == NULL || objects == NULL) {
        fprintf(stderr, "Error: cannot allocate linker working memory.\n");
        free(global_symbols);
        free(output_buffer);
        free(objects);
        return 1;
    }
    output_size = 0U;
    global_symbol_count = 0U;
    total_relocations = 0U;

    result = 0;
    for (i = 0; i < object_count && result == 0; i++) {
        if (read_object(&objects[i], argv[object_argument_index + i]) < 0) {
            result = -1;
        } else if (objects[i].relocation_count >
                   (uint64_t)limits.max_relocations - total_relocations) {
            fprintf(stderr, "Error: relocation capacity (%u) exceeded.\n",
                    limits.max_relocations);
            result = -1;
        } else {
            total_relocations += objects[i].relocation_count;
            if (place_sections(&objects[i], base_address) < 0) result = -1;
        }
    }
    for (i = 0; i < object_count && result == 0; i++) {
        if (collect_symbols(&objects[i], base_address) < 0) result = -1;
    }
    if (result == 0 && ensure_symbols_resolved() < 0) result = -1;
    for (i = 0; i < object_count && result == 0; i++) {
        if (apply_relocations(&objects[i], base_address) < 0) result = -1;
    }
    if (result == 0 && write_output(output_path) < 0) result = -1;

    if (result == 0) {
        printf("elf2bin: generated '%s' (%u bytes) at base 0x%08X\n",
               output_path, output_size, base_address);
    }
    free_objects(objects, object_count);
    free(output_buffer);
    free(global_symbols);
    return result == 0 ? 0 : 1;
}
