/* Motore zona occhi: implementazione (Fase 8). Vedi fx_eyes.h.
 *
 * Registro di 7 effetti dedicati ai 2 pixel occhi in coda alla catena.
 * Il PRNG del blink e' un xorshift32 con seed iniettato: sequenze di blink
 * riproducibili nei test host. Lo stato esposto dai getter (get_look) e'
 * protetto da un seqlock a scrittore unico (il task anim): il lettore
 * ritenta finche' non copia uno stato coerente, senza dipendere da IDF.
 */
#include <string.h>
#include <stdatomic.h>

#include "fx_eyes.h"
#include "fx_color.h"

/* Sfasamento dell'occhio 1 quando desync e' attivo: fase delle timeline
 * periodiche (breathe/color_cycle) e finestra di blink. */
#define EYES_DESYNC_PHASE_US (300ULL * 1000u)
#define EYES_DESYNC_BLINK_US (130ULL * 1000u)

/* Ordine del registro: l'enum DEVE seguire k_effects[] (static_assert sotto). */
enum { E_OFF, E_SOLID, E_BREATHE, E_CYCLE, E_BLINK, E_FLASH, E_GLARE };

static const led_fx_eyes_effect_info_t k_effects[] = {
    { "off", false },
    { "solid", false },
    { "breathe", false },
    { "color_cycle", false },
    { "blink", false },
    { "flash", true },
    { "glare", true },
};
#define EYES_EFFECT_COUNT (sizeof(k_effects) / sizeof(k_effects[0]))
_Static_assert(E_GLARE + 1 == EYES_EFFECT_COUNT,
               "k_effects non allineato all'enum degli effetti occhi");

/* Look base: le modifiche di stato sono immediate, gli istanti (lerp) si
 * ancorano al now_us del render successivo (commit). */
typedef struct {
    uint8_t effect;      /* indice nel registro occhi */
    fx_rgb_t target;     /* colore target del look base */
    fx_rgb_t current;    /* colore corrente (lerp verso target) */
    fx_rgb_t lerp_from;  /* colore di partenza del lerp in corso */
    bool lerp_pending;   /* nuovo target da ancorare al prossimo render */
    uint64_t lerp_start; /* [us, clock iniettato] inizio del lerp */
    uint8_t brightness;  /* 0-255, applicato solo allo stadio di uscita */
    bool blink_overlay;  /* blink casuale sovrapposto al look base */
} eyes_base_t;

/* One-shot: stessa struct per l'istanza attiva e per la richiesta pendente
 * (ancorata al now_us del prossimo render). */
typedef struct {
    bool active;
    bool pending;         /* richiesta in attesa del prossimo render */
    uint8_t idx;          /* indice nel registro (E_FLASH/E_GLARE) */
    uint64_t start_us;    /* [us, clock iniettato] */
    uint32_t duration_ms;
    fx_rgb_t color;       /* colore oneshot a piena scala */
    bool color_set;       /* colore imposto dal piano FSM */
} eyes_oneshot_t;

static eyes_base_t s_base = { .effect = E_OFF, .brightness = 255 };
static eyes_oneshot_t s_oneshot;      /* oneshot attivo */
static eyes_oneshot_t s_oneshot_req;  /* oneshot richiesto, non ancora partito */
static bool s_follow;                 /* gate del piano FSM (default: off) */
static bool s_desync;                 /* sfasamento occhio 1 */
static uint32_t s_rng;                /* xorshift32 del blink */
static uint64_t s_blink_next_us;      /* inizio della prossima finestra di blink */
static uint64_t s_blink_prev_us;      /* inizio della finestra appena conclusa */

/* Seqlock a scrittore unico: pari = stato stabile, dispari = in scrittura. */
static atomic_uint s_seq;

static void look_write_begin(void)
{
    atomic_fetch_add_explicit(&s_seq, 1u, memory_order_acquire);
}

static void look_write_end(void)
{
    atomic_fetch_add_explicit(&s_seq, 1u, memory_order_release);
}

/* ------------------------------------------------------------------ */
/* PRNG blink (xorshift32; lo stato 0 e' morto -> seed di riserva)      */
/* ------------------------------------------------------------------ */

static uint32_t rng32(void)
{
    uint32_t x = s_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_rng = x;
    return x;
}

/* Intervallo casuale tra la fine di un blink e l'inizio del successivo. */
static uint64_t blink_rand_interval_us(void)
{
    uint32_t ms = (uint32_t)FX_EYES_BLINK_MIN_MS +
                  rng32() % (uint32_t)(FX_EYES_BLINK_MAX_MS - FX_EYES_BLINK_MIN_MS + 1u);
    return (uint64_t)ms * 1000u;
}

/* Avanza il schedule del blink (timeline base, occhio 0): una sola volta
 * per render, poi entrambi gli occhi vi si confrontano con il proprio
 * sfasamento. La finestra appena conclusa resta in s_blink_prev_us: con il
 * desync l'occhio 1 la valuta 130 ms dopo la fine, quando il frontier e'
 * gia' avanzato (130 ms « intervallo minimo 2 s: basta una finestra di
 * storia, l'occhio 0 non puo' mai cadervi dentro). */
static void blink_advance(uint64_t now_us)
{
    const uint64_t win = (uint64_t)FX_EYES_BLINK_MS * 1000u;
    while (now_us >= s_blink_next_us + win) {
        s_blink_prev_us = s_blink_next_us;
        s_blink_next_us += win + blink_rand_interval_us();
    }
}

static bool blink_dark(uint64_t t_us)
{
    const uint64_t win = (uint64_t)FX_EYES_BLINK_MS * 1000u;
    return (t_us >= s_blink_next_us && t_us < s_blink_next_us + win) ||
           (t_us >= s_blink_prev_us && t_us < s_blink_prev_us + win);
}

/* ------------------------------------------------------------------ */
/* Timeline dei look base                                               */
/* ------------------------------------------------------------------ */

/* (1 - cos(2*pi*t/T)) / 2 quantizzato a 0-127, via LUT seno: sin^2(pi*t/T). */
static uint8_t breathe_env(uint64_t t_us)
{
    const uint64_t period = (uint64_t)FX_EYES_BREATHE_PERIOD_MS * 1000u;
    uint64_t ph = (t_us % period) * 128u / period; /* 0..127 */
    int s = k_fx_sin[ph];                          /* sin(pi*t/T) >= 0 */
    return (uint8_t)(((s * s) + 63) / 127);
}

static fx_rgb_t scale_env(fx_rgb_t c, uint8_t env)
{
    return (fx_rgb_t){
        (uint8_t)(((uint16_t)c.r * env) / 127u),
        (uint8_t)(((uint16_t)c.g * env) / 127u),
        (uint8_t)(((uint16_t)c.b * env) / 127u),
    };
}

/* Giro hue completo in FX_EYES_CYCLE_PERIOD_MS, saturazione e valore piene. */
static fx_rgb_t cycle_color(uint64_t t_us)
{
    const uint64_t period = (uint64_t)FX_EYES_CYCLE_PERIOD_MS * 1000u;
    uint8_t h = (uint8_t)((t_us % period) * 256u / period);
    return fx_hsv8(h, 255, 255);
}

/* Colore del look base per un occhio alla sua timeline t_us (senza blink). */
static fx_rgb_t base_color_at(uint64_t t_us)
{
    switch (s_base.effect) {
    case E_OFF:
        return (fx_rgb_t){ 0, 0, 0 };
    case E_BREATHE:
        return scale_env(s_base.current, breathe_env(t_us));
    case E_CYCLE:
        return cycle_color(t_us);
    case E_SOLID:
    case E_BLINK: /* le finestre di blink sono applicate a valle */
    default:
        return s_base.current;
    }
}

/* ------------------------------------------------------------------ */
/* Timeline glare                                                       */
/* ------------------------------------------------------------------ */

/* Livello 0-255 del glare alla us t dalla partenza, su una durata totale
 * di duration_ms: salita lineare, mantenimento, strobo 15 Hz, dissolvenza
 * lineare a 0. Fasi in ordine fisso: una durata troppo corta tronca le
 * ultime, la fine resta sempre duration_ms. */
static uint8_t glare_level(uint64_t t_us, uint32_t duration_ms)
{
    const uint64_t t = t_us / 1000u;
    const uint64_t dur = duration_ms;
    const uint64_t fade_start =
        (dur > FX_EYES_GLARE_FADE_MS) ? dur - FX_EYES_GLARE_FADE_MS : 0;
    const uint64_t strobo_start =
        (fade_start > FX_EYES_GLARE_STROBO_MS) ? fade_start - FX_EYES_GLARE_STROBO_MS : 0;
    const uint64_t rise =
        (FX_EYES_GLARE_RISE_MS < strobo_start) ? FX_EYES_GLARE_RISE_MS : strobo_start;

    if (t < rise)
        return (uint8_t)(rise ? (t * 255u) / rise : 0u);
    if (t < strobo_start)
        return 255;
    if (t < fade_start) {
        /* Strobo 15 Hz: periodo 1e6/15 us, acceso per il primo mezzo periodo. */
        const uint64_t period_us = 1000000u / FX_EYES_GLARE_STROBO_HZ;
        return ((t_us % period_us) < period_us / 2u) ? 255 : 0;
    }
    const uint64_t fade = dur - fade_start;
    if (fade == 0)
        return 0;
    return (uint8_t)(((dur - t) * 255u) / fade);
}

static fx_rgb_t scale_level(fx_rgb_t c, uint8_t lvl)
{
    return (fx_rgb_t){
        (uint8_t)(((uint16_t)c.r * lvl) / 255u),
        (uint8_t)(((uint16_t)c.g * lvl) / 255u),
        (uint8_t)(((uint16_t)c.b * lvl) / 255u),
    };
}

static uint8_t oneshot_level(uint64_t t_us)
{
    if (s_oneshot.idx == E_FLASH)
        return 255; /* flash: colore base a piena scala per tutta la durata */
    return glare_level(t_us, s_oneshot.duration_ms);
}

/* ------------------------------------------------------------------ */
/* API del motore                                                       */
/* ------------------------------------------------------------------ */

/* Ricerca per nome con numero massimo di byte letti: usata anche sui campi
 * char[16] del piano FSM (non garantiti NUL-terminati); strncmp si ferma
 * comunque al NUL dei nomi del registro, mai oltre maxlen su name. */
static int find_bounded(const char *name, size_t maxlen)
{
    if (name == NULL)
        return -1;
    for (size_t i = 0; i < EYES_EFFECT_COUNT; i++)
        if (strncmp(k_effects[i].name, name, maxlen) == 0)
            return (int)i;
    return -1;
}

void fx_eyes_init(uint32_t seed, bool desync)
{
    look_write_begin();
    memset(&s_base, 0, sizeof(s_base));
    s_base.brightness = 255;
    memset(&s_oneshot, 0, sizeof(s_oneshot));
    memset(&s_oneshot_req, 0, sizeof(s_oneshot_req));
    s_follow = false;
    s_desync = desync;
    s_rng = (seed != 0) ? seed : 0x9E3779B9u;
    s_blink_prev_us = 0;
    s_blink_next_us = blink_rand_interval_us(); /* primo blink a 2-8 s da t=0 */
    look_write_end();
}

int fx_eyes_find_effect(const char *name)
{
    return find_bounded(name, 64); /* nomi del registro: 11 char al massimo */
}

size_t fx_eyes_get_effects(const led_fx_eyes_effect_info_t **out)
{
    if (out != NULL)
        *out = k_effects;
    return EYES_EFFECT_COUNT;
}

esp_err_t fx_eyes_validate_oneshot(int idx, uint32_t duration_ms)
{
    if (idx < 0 || (size_t)idx >= EYES_EFFECT_COUNT || !k_effects[idx].oneshot)
        return ESP_ERR_INVALID_ARG;
    if (idx == E_GLARE) {
        if (duration_ms == 0 || duration_ms > FX_EYES_GLARE_MAX_MS)
            return ESP_ERR_INVALID_ARG;
        return ESP_OK;
    }
    return (duration_ms <= FX_EYES_GLARE_MAX_MS) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

void fx_eyes_set_effect_idx(int idx)
{
    if (idx < 0 || (size_t)idx >= EYES_EFFECT_COUNT || k_effects[idx].oneshot)
        return; /* il wrapper pubblico ha gia' validato */
    look_write_begin();
    s_base.effect = (uint8_t)idx; /* timeline assolute: nessuna ripartenza */
    look_write_end();
}

void fx_eyes_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    look_write_begin();
    s_base.target = (fx_rgb_t){ r, g, b };
    s_base.lerp_pending = true;
    look_write_end();
}

void fx_eyes_set_brightness(uint8_t brightness)
{
    look_write_begin();
    s_base.brightness = brightness;
    look_write_end();
}

void fx_eyes_set_blink_overlay(bool enable)
{
    look_write_begin();
    s_base.blink_overlay = enable;
    look_write_end();
}

void fx_eyes_set_follow(bool enable)
{
    look_write_begin();
    s_follow = enable;
    look_write_end();
}

/* Richiesta di oneshot (lock gia' preso): parte al prossimo render. */
static void oneshot_request(int idx, uint32_t duration_ms, bool color_set,
                            const fx_rgb_t *color)
{
    memset(&s_oneshot_req, 0, sizeof(s_oneshot_req));
    s_oneshot_req.pending = true;
    s_oneshot_req.idx = (uint8_t)idx;
    s_oneshot_req.duration_ms = duration_ms;
    s_oneshot_req.color_set = color_set;
    if (color_set)
        s_oneshot_req.color = *color;
}

esp_err_t fx_eyes_play_oneshot(int idx, uint32_t duration_ms, bool color_set,
                               const fx_rgb_t *color)
{
    esp_err_t err = fx_eyes_validate_oneshot(idx, duration_ms);
    if (err != ESP_OK)
        return err;
    if (idx == E_FLASH && duration_ms == 0)
        duration_ms = FX_EYES_FLASH_MS; /* default di specifica */
    look_write_begin();
    oneshot_request(idx, duration_ms, color_set, color);
    look_write_end();
    return ESP_OK;
}

esp_err_t fx_eyes_apply_fsm_plan(const led_fx_eyes_plan_t *plan)
{
    if (plan == NULL || (unsigned)plan->kind > (unsigned)LED_FX_EYES_PLAN_OFF)
        return ESP_ERR_INVALID_ARG;
    if (!s_follow)
        return ESP_OK; /* gate FSM: piano ignorato, nessun cambio */

    switch (plan->kind) {
    case LED_FX_EYES_PLAN_IDLE:
    case LED_FX_EYES_PLAN_COOLDOWN: {
        /* Look idle: il lerp del colore rende progressiva la transizione
         * di ritorno (COOLDOWN) dal colore scare. */
        int idx = find_bounded(plan->idle_effect, sizeof(plan->idle_effect));
        if (idx < 0 || k_effects[idx].oneshot)
            return ESP_ERR_INVALID_ARG;
        look_write_begin();
        s_base.effect = (uint8_t)idx;
        s_base.target = (fx_rgb_t){ plan->idle_r, plan->idle_g, plan->idle_b };
        s_base.lerp_pending = true;
        s_base.brightness = plan->idle_brightness;
        s_base.blink_overlay = true;
        look_write_end();
        return ESP_OK;
    }
    case LED_FX_EYES_PLAN_TRIGGERED:
        if (plan->duration_ms == 0 || plan->duration_ms > FX_EYES_GLARE_MAX_MS)
            return ESP_ERR_INVALID_ARG;
        look_write_begin();
        s_base.target = (fx_rgb_t){ plan->scare_r, plan->scare_g, plan->scare_b };
        s_base.lerp_pending = true;
        s_base.brightness = plan->scare_brightness;
        {
            fx_rgb_t scare = s_base.target;
            oneshot_request(E_GLARE, plan->duration_ms, true, &scare);
        }
        look_write_end();
        return ESP_OK;
    case LED_FX_EYES_PLAN_OFF:
    default:
        look_write_begin();
        s_base.effect = E_OFF;
        look_write_end();
        return ESP_OK;
    }
}

uint8_t fx_eyes_brightness(void)
{
    return s_base.brightness; /* lettura allineata a 8 bit, atomico di fatto */
}

static void lerp_step(uint64_t now_us)
{
    const uint32_t trans_us = (uint32_t)FX_EYES_TRANSITION_MS * 1000u;
    uint64_t dt = (now_us > s_base.lerp_start) ? now_us - s_base.lerp_start : 0;
    if (trans_us == 0 || dt >= trans_us) {
        s_base.current = s_base.target;
        return;
    }
    const fx_rgb_t *f = &s_base.lerp_from;
    const fx_rgb_t *t = &s_base.target;
    s_base.current = (fx_rgb_t){
        (uint8_t)((int)f->r + (((int)t->r - (int)f->r) * (int)dt) / (int)trans_us),
        (uint8_t)((int)f->g + (((int)t->g - (int)f->g) * (int)dt) / (int)trans_us),
        (uint8_t)((int)f->b + (((int)t->b - (int)f->b) * (int)dt) / (int)trans_us),
    };
}

void fx_eyes_render(fx_rgb_t px[2], uint64_t now_us)
{
    look_write_begin();

    /* 1. Ancoraggi al clock del frame corrente: i comandi applicati dopo il
     *    render precedente (drain della coda) partono da qui. */
    if (s_base.lerp_pending) {
        s_base.lerp_from = s_base.current;
        s_base.lerp_start = now_us;
        s_base.lerp_pending = false;
    }
    if (s_oneshot_req.pending) {
        s_oneshot = s_oneshot_req;
        s_oneshot.start_us = now_us;
        s_oneshot.active = true;
        memset(&s_oneshot_req, 0, sizeof(s_oneshot_req));
        if (!s_oneshot.color_set)
            s_oneshot.color = s_base.target; /* flash: colore base a piena scala */
    }

    /* 2. Schedule del blink e avanzamento del lerp del colore base. */
    blink_advance(now_us);
    lerp_step(now_us);

    /* 3. Fine oneshot: riprende il look base (nessun reset del lerp). */
    if (s_oneshot.active &&
        now_us - s_oneshot.start_us >= (uint64_t)s_oneshot.duration_ms * 1000u)
        s_oneshot.active = false;

    /* 4. Render dei due occhi. L'occhio 1 (se desync) e' in ritardo: 300 ms
     *    sulla fase di breathe/color_cycle, 130 ms sulla finestra di blink. */
    bool blink_on = s_base.blink_overlay || s_base.effect == E_BLINK;
    if (s_oneshot.active) {
        /* One-shot in simmetria: stessa timeline, blink sospeso. */
        uint8_t lvl = oneshot_level(now_us - s_oneshot.start_us);
        px[0] = scale_level(s_oneshot.color, lvl);
        px[1] = px[0];
    } else {
        uint64_t t_phase = (s_desync && now_us > EYES_DESYNC_PHASE_US)
                               ? now_us - EYES_DESYNC_PHASE_US
                               : now_us;
        uint64_t t_blink = (s_desync && now_us > EYES_DESYNC_BLINK_US)
                               ? now_us - EYES_DESYNC_BLINK_US
                               : now_us;
        px[0] = base_color_at(now_us);
        px[1] = base_color_at(t_phase);
        if (blink_on) {
            const fx_rgb_t black = { 0, 0, 0 };
            if (blink_dark(now_us))
                px[0] = black;
            if (blink_dark(t_blink))
                px[1] = black;
        }
    }

    look_write_end();
}

esp_err_t fx_eyes_get_look(led_fx_eyes_look_t *out)
{
    if (out == NULL)
        return ESP_ERR_INVALID_ARG;

    unsigned s1, s2;
    do {
        do {
            s1 = atomic_load_explicit(&s_seq, memory_order_acquire);
        } while (s1 & 1u); /* scrittura in corso: ritenta */

        out->brightness = s_base.brightness;
        out->blink_overlay = s_base.blink_overlay;
        out->oneshot_active = s_oneshot.active;
        out->r = s_base.target.r;
        out->g = s_base.target.g;
        out->b = s_base.target.b;
        const char *src = k_effects[s_base.effect].name;
        size_t i = 0;
        for (; i < sizeof(out->effect) - 1 && src[i] != '\0'; i++)
            out->effect[i] = src[i];
        out->effect[i] = '\0'; /* sempre NUL-terminato, mai copy oltre 16 */

        s2 = atomic_load_explicit(&s_seq, memory_order_acquire);
    } while (s1 != s2); /* stato cambiato sotto la copia: ritenta */
    return ESP_OK;
}
