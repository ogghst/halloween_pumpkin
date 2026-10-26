/* Pure TLV codec for the "mqtt" provisioning endpoint (see prov_mqtt_codec.h). */
#include <string.h>

#include "prov_mqtt_codec.h"

/* Copy a record value into a fixed string buffer, truncating to keep the
 * NUL inside: vlen < size -> vlen bytes + terminator, else size-1 bytes. */
static void dec_str(char *dst, size_t size, const uint8_t *val, uint16_t vlen)
{
    size_t n = (vlen < size) ? (size_t)vlen : size - 1u;
    memcpy(dst, val, n);
    dst[n] = '\0';
}

esp_err_t prov_mqtt_decode(const uint8_t *buf, size_t len, prov_mqtt_msg_t *out)
{
    if (!buf || !out || len < 1u) {
        return ESP_ERR_INVALID_ARG;
    }
    if (buf[0] != PROV_MQTT_TLV_VERSION) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));

    size_t pos = 1u;
    while (pos < len) {
        if (len - pos < 3u) {
            return ESP_ERR_INVALID_ARG; /* truncated record header */
        }
        uint8_t type = buf[pos];
        uint16_t vlen = (uint16_t)(buf[pos + 1u] | ((uint16_t)buf[pos + 2u] << 8));
        pos += 3u;
        if ((size_t)vlen > len - pos) {
            return ESP_ERR_INVALID_ARG; /* value runs past the buffer */
        }
        const uint8_t *val = &buf[pos];

        switch (type) {
        case PROV_MQTT_TLV_HOST:
            dec_str(out->host, sizeof(out->host), val, vlen);
            out->has_host = true;
            break;
        case PROV_MQTT_TLV_PORT:
            if (vlen != 2u) {
                return ESP_ERR_INVALID_ARG;
            }
            out->port = (uint16_t)(val[0] | ((uint16_t)val[1] << 8));
            out->has_port = true;
            break;
        case PROV_MQTT_TLV_USER:
            dec_str(out->user, sizeof(out->user), val, vlen);
            out->has_user = true;
            break;
        case PROV_MQTT_TLV_PASS:
            dec_str(out->pass, sizeof(out->pass), val, vlen);
            out->has_pass = true;
            break;
        case PROV_MQTT_TLV_TLS:
            if (vlen != 1u || val[0] > 1u) {
                return ESP_ERR_INVALID_ARG;
            }
            out->tls = val[0] != 0u;
            out->has_tls = true;
            break;
        default:
            break; /* unknown type: skipped, parsing continues */
        }
        pos += vlen;
    }
    return ESP_OK;
}

/* Append one string record (type, len u16 LE, value with NUL). */
static esp_err_t enc_str(uint8_t *buf, size_t cap, size_t *pos,
                         prov_mqtt_tlv_type_t type, const char *s, size_t size)
{
    size_t slen = strnlen(s, size);
    if (slen == size) {
        return ESP_ERR_INVALID_ARG; /* not NUL-terminated */
    }
    size_t vlen = slen + 1u; /* value carries the NUL */
    if (cap - *pos < 3u + vlen) {
        return ESP_ERR_NO_MEM;
    }
    buf[*pos] = (uint8_t)type;
    buf[*pos + 1u] = (uint8_t)(vlen & 0xFFu);
    buf[*pos + 2u] = (uint8_t)(vlen >> 8);
    memcpy(&buf[*pos + 3u], s, vlen);
    *pos += 3u + vlen;
    return ESP_OK;
}

esp_err_t prov_mqtt_encode(const prov_mqtt_msg_t *in, uint8_t *buf,
                           size_t buf_len, size_t *out_len)
{
    if (!in || !buf || !out_len || buf_len < 1u) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t pos = 0u;
    buf[pos++] = PROV_MQTT_TLV_VERSION;

    esp_err_t err = ESP_OK;
    if (in->has_host) {
        err = enc_str(buf, buf_len, &pos, PROV_MQTT_TLV_HOST,
                      in->host, sizeof(in->host));
    }
    if (err == ESP_OK && in->has_port) {
        if (buf_len - pos < 5u) {
            err = ESP_ERR_NO_MEM;
        } else {
            buf[pos] = PROV_MQTT_TLV_PORT;
            buf[pos + 1u] = 2u;
            buf[pos + 2u] = 0u;
            buf[pos + 3u] = (uint8_t)(in->port & 0xFFu);
            buf[pos + 4u] = (uint8_t)(in->port >> 8);
            pos += 5u;
        }
    }
    if (err == ESP_OK && in->has_user) {
        err = enc_str(buf, buf_len, &pos, PROV_MQTT_TLV_USER,
                      in->user, sizeof(in->user));
    }
    if (err == ESP_OK && in->has_pass) {
        err = enc_str(buf, buf_len, &pos, PROV_MQTT_TLV_PASS,
                      in->pass, sizeof(in->pass));
    }
    if (err == ESP_OK && in->has_tls) {
        if (buf_len - pos < 4u) {
            err = ESP_ERR_NO_MEM;
        } else {
            buf[pos] = PROV_MQTT_TLV_TLS;
            buf[pos + 1u] = 1u;
            buf[pos + 2u] = 0u;
            buf[pos + 3u] = in->tls ? 1u : 0u;
            pos += 4u;
        }
    }
    if (err != ESP_OK) {
        return err;
    }

    *out_len = pos;
    return ESP_OK;
}
