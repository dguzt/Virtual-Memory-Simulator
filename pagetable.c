//
// Created by jhair on 5/11/19.
//

/* pagetable.c - Modela la tabla de páginas virtuales usada en cada referencia
 * para buscar el pte_t y saber si la página física buscada se encuentra en memoria
 * o no. Sorprendentemente, la tabla de 2do nivel y una tabla de 1er nivel optimizada
 * parecen tener el mismo desempeño (performance)*/

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#include "vmsim.h"
#include "util.h"
#include "options.h"
#include "pagetable.h"
#include "stats.h"

/* Estructura auxiliar en caso vfnBits sea grande, se utilizan los niveles de tabla de páginas */
typedef struct _pagatable_level {
    uint size;
    uint logSize;
    bool_t isLeaf;
} pagetable_level_t;


/* Define la tabla de páginas multinivel. Esto define cuán largo puede
 * ser cada nivel. La función pagetableInit() puede reducir la medida en
 * la variable chosenOpts (recibido por línea de comandos como argumento) */
pagetable_level_t levels[3] = {
        {4096, 12, FALSE},  /* levels[0] posee 12 bits. Si vfnBits excede estos 12 bits, necesitará niveles adicionales    */
        {4096, 12, FALSE},  /* levels[1] posee 12 bits. Si vfnBits excede estos 12 bits, necesitará niveles adicionales    */
        {256 , 8 , TRUE }   /* levels[2] posee 8 bits. El máximo valor de vfnBits que puede ser manejado será 12+12+8=32   */
};

/* vfnBits es el número de bits en el Número de Marcos Virtuales (vfn)
 * vfnBits debe ser la suma de los campos 'logSize' y todos los niveles
 * */
uint vfnBits;

/* Estructura representativa de nuestra tabla de páginas multinivel */
typedef struct _pagetable {
    void **table; //Si es de bajo nivel, array de pte_t pointers. En otro caso, es array de pagetable_t pointers
    int level;
} pagetable_t;

/* rootTable->table es la tabla de página actual.
 * Usa pte_t *pte = (pte_t *)rootTable->table[i]
 * para acceder a cada entrada pte.
 * */
static pagetable_t *rootTable;

pte_t *pagetableLookupHelper(uint vfn, uint bits, uint maskedVFN, pagetable_t *pages, ref_kind_t type);
pte_t *pagetableNewPTE(uint vfn);
pagetable_t *pagetableNewTable(int level);
void pagetableTestEntry(uint vfn, int l1, int l2);
// inline uint getBits(uint bitschain, int position, int nbits); // repetido

/******************************************************************************/
/******************************  IMPLEMENTACIÓN  ******************************/

void pagetableInit() {
    uint pageBits;
    pageBits = log_2(chosenOpts.pageSize);

    if (pageBits == -1) {
        fprintf(stderr, "[ERROR-PAGETABLE]: Pagesize debe ser potencia de 2\n");
        abort();
    }

    vfnBits = addressSpaceBits - pageBits;

    uint bits = 0; int level = 0;
    while (TRUE) {
        bits += levels[level].logSize;
        if (bits >= vfnBits) break;
        level++;
    }

    levels[level].logSize   = levels[level].logSize - (bits - vfnBits);
    levels[level].size      = pow_2(levels[level].logSize);
    levels[level].isLeaf    = TRUE;

    if (chosenOpts.test) {
        printf("[TEST -PAGETABLE]: vfnBits=%d, %d level table\n", vfnBits, level+1);
        for (int i=0; i<=level; i++) {
            printf("[TEST -PAGETABLE]: level %d: %u bits (%u entries)\n", i, levels[i].logSize, levels[i].size);
        }
    }

    rootTable = pagetableNewTable(0);
}

pte_t *pagetableLookupVirtualAddress(uint vfn, ref_kind_t type) {
    return pagetableLookupHelper(vfn, 0, vfn, rootTable, type);
}
/******************************************************************************/

pagetable_t *pagetableNewTable(int level) {
    pagetable_level_t *config = &levels[level];
    assert(config);

    pagetable_t *table = malloc(sizeof(struct _pagetable));
    assert(table);

    table->table = calloc(config->size, sizeof(void*));
    assert(table->table);

    table->level = level;
    return table;
}

pte_t *pagetableLookupHelper(uint vfn, uint bits, uint maskedVFN, pagetable_t *pages, ref_kind_t type) {
    int logSize = levels[pages->level].logSize;
    uint index = getBits(maskedVFN, vfnBits-(1+bits), logSize);

    #ifdef DEBUG
        printf("[DEBUG-PAGETABLE]: vfn(0x%x), Llamando a index[%d]=getBits(maskedVFN(0x%x), vfnBits(%d)-(1-bits(%d)), logSize(%d)\n)",
        index, maskedVFN, vfnBits-1(1+bits), vfnBits-bits);
    #endif

    bits += logSize;
    maskedVFN = getBits(maskedVFN, vfnBits-(1+bits), vfnBits-bits);

    #ifdef DEBUG
        printf("[DEBUG-PAGETABLE]: Una vez más, llamando a index[%d]=getBits(maskedVFN(0x%x), vfnBits(%d)-(1-bits(%d)), logSize(%d)\n)",
        index, maskedVFN, vfnBits, vfnBits-bits);
    #endif

    if (levels[pages->level].isLeaf) {
        if (pages->table[index] == NULL) {
            // Compulsory miss - primer acceso
            statsCompulsory(type);
            pages->table[index] = (void*) pagetableNewPTE(vfn);
        }
        return (pte_t*)(pages->table[index]);
    }

    else {
        if (pages->table[index] == NULL) {
            pages->table[index] = pagetableNewTable(pages->level+1);
        }
        return pagetableLookupHelper(vfn, bits, maskedVFN, pages->table[index], type);
    }
}

pte_t *pagetableNewPTE(uint vfn) {
    pte_t *pte = (pte_t*)(malloc(sizeof(pte_t)));
    assert(pte);

    pte->vfn        = vfn;      // vfn guardado en esta nueva entrada de la tabla de página
    pte->pfn        = -1;       // nueva entrada aún no apunta a una página física
    pte->valid      = FALSE;    // nueva entrada con pfn=-1 es valid=FALSE, hasta apuntar a una pfn en memoria principal
    pte->modified   = FALSE;    // nueva entrada lista para apuntar a espacio nuevo (no modificado)
    pte->reference  = 0;        // nueva entrada no tiene referencias aún

    return pte;
}

void pagetableTest() {
    printf("[TEST -PAGETABLE]: Testeando Pagatables...\n");
    pagetableInit();
    assert(rootTable);

    if (vfnBits == 22) {
        pagetableTestEntry(0   , 0, 0   );
        pagetableTestEntry(1023, 0, 1023);
        pagetableTestEntry(1024, 1, 0   );
        pagetableTestEntry((1 << vfnBits) - 1   , levels[0].size-1, levels[1].size-1);
        pagetableTestEntry((1 << vfnBits) - 2   , levels[0].size-1, levels[1].size-2);
        pagetableTestEntry((1 << vfnBits) - 1024, levels[0].size-1, 0               );
        pagetableTestEntry((1 << vfnBits) - 1025, levels[0].size-2, levels[1].size-1);
    }
}

void pagetableTestEntry(uint vfn, int l1, int l2) {
    printf("[TEST -PAGETABLE]: Buscando vfn=%u\n", vfn);

    pte_t *pte = pagetableLookupVirtualAddress(vfn, REF_KIND_CODE);
    assert(pte && pte->vfn==vfn);
    assert(rootTable->table[l1]);

    assert(((pagetable_t*)rootTable->table[l1])->table[l2]);
    assert(((pte_t*)((pagetable_t*)rootTable->table[l1])->table[l2])->vfn == vfn);
}

void pagetableDump() {
    assert(rootTable);
    assert(rootTable->level == 0);
    uint locaVFNBits = addressSpaceBits - log_2(chosenOpts.pageSize);
    uint ptSize = pow_2(locaVFNBits);

    printf("[INFO -PAGETABLE]: Campos de Tabla de Páginas actual PTE. valid:vfn:pfn:modified:reference:counter\n");
    for (uint i=0; i<ptSize; i++) {
        if (rootTable->table[i]) {
            pte_t *pte = (pte_t*) rootTable->table[i];
            printf("[INFO -PAGETABLE]: table[0x%x]:%d:0x%x:0%x:%d:%d:%d\n",
                    i,
                    pte->valid,
                    pte->vfn,
                    pte->pfn,
                    pte->modified,
                    pte->reference,
                    pte->counter);
        }
    }
}
/******************************************************************************/
