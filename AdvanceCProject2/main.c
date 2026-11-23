#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "custom_allocator.h"


#define MAX_VARS   50
#define MAX_BLOCKS 50

typedef struct {
    char  name[32];
    char  type[8];    
    int   addr;       
    int   i;         
    float f;           
    char  c;          
    int   points_to;  
} Var;


typedef struct {
    int   addr;      
    int   size;
    int   is_free;
    int   value;      
    void *real;        
} Block;

Var   vars[MAX_VARS];
int   var_count = 0;
int   next_stack_addr = 0x1000;

Block blocks[MAX_BLOCKS];
int   block_count = 0;
int   next_heap_addr = 0x5000;

char  message[256] = "Welcome! Type something like: int x = 42;";


void reset_all(void) {
    var_count = 0;
    block_count = 0;
    next_stack_addr = 0x1000;
    next_heap_addr = 0x5000;
    ca_init();
}

Var *find_var(const char *name) {
    for (int k = 0; k < var_count; k++)
        if (strcmp(vars[k].name, name) == 0) return &vars[k];
    return NULL;
}

Block *find_block(int addr) {
    for (int k = 0; k < block_count; k++)
        if (blocks[k].addr == addr) return &blocks[k];
    return NULL;
}

Var *make_var(const char *name, const char *type) {
    Var *v = find_var(name);
    if (v) { strcpy(v->type, type); return v; }
    if (var_count >= MAX_VARS) return NULL;

    v = &vars[var_count++];
    memset(v, 0, sizeof(Var));
    strcpy(v->name, name);
    strcpy(v->type, type);
    v->addr = next_stack_addr;
    next_stack_addr += 8;              
    return v;
}

void clear_screen(void) {
    printf("\033[H\033[J");
}

void show_memory(void) {
    printf("=========== STACK ===========\n");
    if (var_count == 0) printf("  (empty)\n");
    for (int k = 0; k < var_count; k++) {
        Var *v = &vars[k];
        printf("  0x%04X  %-5s %-8s = ", v->addr, v->type, v->name);
        if      (strcmp(v->type, "int")   == 0) printf("%d\n", v->i);
        else if (strcmp(v->type, "float") == 0) printf("%.2f\n", v->f);
        else if (strcmp(v->type, "char")  == 0) printf("'%c'\n", v->c);
        else if (v->points_to == 0)             printf("NULL\n");
        else                                    printf("-> 0x%04X\n", v->points_to);
    }

    printf("\n=========== HEAP ============\n");
    if (block_count == 0) printf("  (empty)\n");
    for (int k = 0; k < block_count; k++) {
        Block *b = &blocks[k];
        if (b->is_free) printf("  0x%04X  %3d bytes  [FREED]\n", b->addr, b->size);
        else            printf("  0x%04X  %3d bytes  [IN USE]  value = %d\n", b->addr, b->size, b->value);
    }

    printf("\n>> %s\n\n", message);
}

void show_help(void) {
    printf("Type one of these:\n");
    printf("  int x = 42;           char c = 'A';        float f = 3.5;\n");
    printf("  int* p = &x;          int* p = malloc(16);\n");
    printf("  *p = 99;              free(p);\n");
    printf("  map  stats  leaks  log  validate  reset  help  exit\n\n");
}

void wait_for_enter(void) {
    printf("\nPress [Enter] to go back...");
    int ch;
    while ((ch = getchar()) != '\n' && ch != EOF);
}

void do_free(const char *name) {
    Var *p = find_var(name);
    if (p == NULL || strcmp(p->type, "int*") != 0) {
        snprintf(message, sizeof(message), "'%s' is not a pointer.", name);
        return;
    }
    Block *b = find_block(p->points_to);
    if (b == NULL || b->is_free) {
        snprintf(message, sizeof(message), "Double free or bad pointer: 0x%X", p->points_to);
        return;
    }
    ca_free(b->real);         
    b->is_free = 1;
    snprintf(message, sizeof(message), "Freed 0x%X.", b->addr);
}

void do_write(const char *name, int number) {
    Var *p = find_var(name);
    if (p == NULL || strcmp(p->type, "int*") != 0) {
        snprintf(message, sizeof(message), "'%s' is not a pointer.", name);
        return;
    }
    Block *b = find_block(p->points_to);
    if (b == NULL || b->is_free) {
        snprintf(message, sizeof(message), "Use-after-free! 0x%X is not in use.", p->points_to);
        return;
    }
    b->value = number;
    snprintf(message, sizeof(message), "Stored %d at 0x%X.", number, b->addr);
}

void do_malloc(const char *name, int size) {
    if (block_count >= MAX_BLOCKS) {
        snprintf(message, sizeof(message), "Too many heap blocks.");
        return;
    }
    void *real = ca_malloc(size);     
    if (real == NULL) {
        snprintf(message, sizeof(message), "Out of memory (%d bytes).", size);
        return;
    }
    Block *b = &blocks[block_count++];
    b->addr = next_heap_addr;
    b->size = size;
    b->is_free = 0;
    b->value = 0;
    b->real = real;
    next_heap_addr += (size + 7) / 8 * 8;     

    Var *p = make_var(name, "int*");
    if (p) p->points_to = b->addr;
    snprintf(message, sizeof(message), "malloc(%d) gave 0x%X. '%s' points to it.", size, b->addr, name);
}

void do_declare(const char *type, const char *name, const char *value, int has_value) {
    Var *v;

    if (strcmp(type, "int*") == 0) {
        if (has_value && value[0] == '&') {
            Var *target = find_var(value + 1);
            if (target == NULL) {
                snprintf(message, sizeof(message), "Variable '%s' not found.", value + 1);
                return;
            }
            v = make_var(name, "int*");
            v->points_to = target->addr;
            snprintf(message, sizeof(message), "'%s' points to '%s' at 0x%X.", name, target->name, target->addr);
        } else {
            v = make_var(name, "int*");
            v->points_to = 0;
            snprintf(message, sizeof(message), "Pointer '%s' points to NULL.", name);
        }
    } else if (strcmp(type, "int") == 0) {
        v = make_var(name, "int");
        v->i = has_value ? atoi(value) : 0;
        snprintf(message, sizeof(message), "int '%s' = %d at 0x%X.", name, v->i, v->addr);
    } else if (strcmp(type, "float") == 0) {
        v = make_var(name, "float");
        v->f = has_value ? (float)atof(value) : 0;
        snprintf(message, sizeof(message), "float '%s' = %.2f at 0x%X.", name, v->f, v->addr);
    } else if (strcmp(type, "char") == 0) {
        v = make_var(name, "char");
        v->c = (has_value && value[0] == '\'') ? value[1] : 0;
        snprintf(message, sizeof(message), "char '%s' = '%c' at 0x%X.", name, v->c, v->addr);
    } else {
        snprintf(message, sizeof(message), "Unsupported type '%s'. Use int, float, char, int*", type);
    }
}

void run_statement(char *line) {

    int len = strlen(line);
    while (len > 0 && (line[len - 1] == ';' || line[len - 1] == ' '))
        line[--len] = '\0';

    char type[16], name[32], value[64];
    int number;

    if (strncmp(line, "free(", 5) == 0 && sscanf(line, "free(%31[^)])", name) == 1) {
        do_free(name);
    }
    else if (line[0] == '*' && sscanf(line, "*%31s = %d", name, &number) == 2) {
        do_write(name, number);
    }
    else {
        int count = sscanf(line, "%15s %31s = %63s", type, name, value);
        if (count < 2) {
            snprintf(message, sizeof(message), "Could not understand: %s", line);
            return;
        }

        char *var_name = name;
        if (strcmp(type, "int") == 0 && name[0] == '*') {
            strcpy(type, "int*");
            var_name = name + 1;
        }

        if (strcmp(type, "int*") == 0 && count == 3 && strncmp(value, "malloc(", 7) == 0) {
            int size = 4;
            sscanf(value, "malloc(%d)", &size);
            do_malloc(var_name, size);
        } else {
            do_declare(type, var_name, value, count == 3);
        }
    }
}


int main(void) {
    char input[256];
    reset_all();

    while (1) {
        clear_screen();
        show_memory();
        show_help();

        printf("c-memory > ");
        if (fgets(input, sizeof(input), stdin) == NULL) break;

        input[strcspn(input, "\r\n")] = '\0';
        char *line = input;
        while (*line == ' ' || *line == '\t') line++;
        if (*line == '\0') continue;

        if      (strcmp(line, "exit") == 0)     break;
        else if (strcmp(line, "map") == 0)      { ca_dump_heap();      wait_for_enter(); }
        else if (strcmp(line, "stats") == 0)    { ca_dump_stats();     wait_for_enter(); }
        else if (strcmp(line, "leaks") == 0)    { ca_check_leaks();    wait_for_enter(); }
        else if (strcmp(line, "log") == 0)      { ca_dump_alloc_log(); wait_for_enter(); }
        else if (strcmp(line, "validate") == 0) { ca_validate_heap();  wait_for_enter(); }
        else if (strcmp(line, "help") == 0)     { show_help();         wait_for_enter(); }
        else if (strcmp(line, "reset") == 0)    { reset_all(); snprintf(message, sizeof(message), "Everything was reset."); }
        else run_statement(line);
    }

    printf("\n--- Final leak check ---\n");
    ca_check_leaks();
    printf("Goodbye!\n");
    return 0;
}
