/* Registro entita' HA: storage statico dei descrittori (puro).
 * I descrittori puntano a memoria del caller: nessuna copia, il caller
 * deve garantire che key/name/options restino validi. */
#include "ha_registry.h"
#include <string.h>

static ha_entity_desc_t s_entities[HA_ENTITY_MAX];
static size_t s_count;

int ha_registry_reset(void)
{
    /* Nessuna risorsa da rilasciare: i desc puntano a memoria caller. */
    s_count = 0;
    return 0;
}

int ha_registry_add(const ha_entity_desc_t *d)
{
    if (d == NULL || d->key == NULL || d->name == NULL)
        return -1;
    if (strlen(d->key) >= HA_KEY_MAX)
        return -1;
    if ((int)d->type < (int)HA_ET_LIGHT || (int)d->type > (int)HA_ET_BUTTON)
        return -1;
    if ((int)d->category < (int)HA_CAT_NONE || (int)d->category > (int)HA_CAT_DIAGNOSTIC)
        return -1;
    if (s_count >= HA_ENTITY_MAX)
        return -1;
    s_entities[s_count] = *d;
    s_count++;
    return 0;
}

size_t ha_registry_count(void)
{
    return s_count;
}

const ha_entity_desc_t *ha_registry_at(size_t i)
{
    if (i >= s_count)
        return NULL;
    return &s_entities[i];
}
