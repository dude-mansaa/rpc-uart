/*
 * Copyright (c) 2014-2018 Cesanta Software Limited
 * All rights reserved
 *
 * Licensed under the Apache License, Version 2.0 (the ""License"");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an ""AS IS"" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "mgos_rpc_channel_uart.h"
#include "mg_rpc.h"
#include "mgos_rpc.h"

#include "mgos_debug.h"
#include "mgos_sys_config.h"
#include "mgos_uart.h"
#include "mgos_utils.h"

#include "common/cs_crc32.h"
#include "common/cs_dbg.h"
#include "common/mbuf.h"
#include "common/str_util.h"

#define EOF_CHAR "\x04"
#define FRAME_DELIMETER "\"\"\""
#define FRAME_DELIMETER_LEN 3

struct mg_rpc_channel_uart_data {
  int uart_no;
  unsigned int wait_for_start_frame : 1;
  unsigned int waiting_for_start_frame : 1;
  unsigned int connected : 1;
  unsigned int sending : 1;
  unsigned int sending_user_frame : 1;
  unsigned int resume_uart : 1;
  struct mbuf recv_mbuf;
  struct mbuf send_mbuf;
};

/*
 * mgos client expects the following sequence:
 *
 *        MGOS              DEVICE
 *        -->  EOF_CHAR"""        (mgos sends continuosly, expecting """)
 *        <--  EOF_CHAR"""        (device replies with """ saying it's ready)
 *        -->  """{request_frame}"""  (mgos sends a frame)
 *                                    at this point we disable UART logs
 *        <--  """{response_frame}""" (device responds)
 *                                    at this point we re-enable UART logs
 *
 * Our side (a device side) must keep UART disabled after we have received
 * EOF_CHAR""" marker, and until we have sent a response.
 *
 * Note that when we have sent a """ ready marker, some time may pass but we
 * have to keep the UART disabled. That's why `chd->sending_user_frame` flag
 * is introduced: it is set only when a frame has been sent by the user code.
 * Note that the user handler may call LOG, so it's important to keep
 * UART disabled during RPC callback execution.
 */
void mg_rpc_channel_uart_dispatcher(int uart_no, void *arg) {
  struct mg_rpc_channel *ch = (struct mg_rpc_channel *) arg;
  struct mg_rpc_channel_uart_data *chd =
      (struct mg_rpc_channel_uart_data *) ch->channel_data;
  size_t rx_av = mgos_uart_read_avail(uart_no);
  if (rx_av > 0) {
    size_t flen = 0;
    const char *end;
    struct mbuf *urxb = &chd->recv_mbuf;

    mgos_uart_read_mbuf(uart_no, urxb, rx_av);
    while ((end = c_strnstr(urxb->buf, FRAME_DELIMETER, urxb->len)) != NULL) {
      flen = (end - urxb->buf);
      if (flen != 0) {
        struct mg_str f = mg_mk_str_n((const char *) urxb->buf, flen);
        /*
         * EOF_CHAR is used to turn off interactive console. If the frame is
         * just EOF_CHAR by itself, we'll immediately send a frame containing
         * eof_char in response (since the frame isn't valid anyway);
         * otherwise we'll handle the frame.
         */
        if (mg_vcmp(&f, EOF_CHAR) == 0) {
          chd->waiting_for_start_frame = false;
          if (!chd->connected) {
            chd->connected = true;
            ch->ev_handler(ch, MG_RPC_CHANNEL_OPEN, NULL);
          }
          mbuf_append(&chd->send_mbuf, FRAME_DELIMETER, FRAME_DELIMETER_LEN);
          mbuf_append(&chd->send_mbuf, EOF_CHAR, 1);
          mbuf_append(&chd->send_mbuf, FRAME_DELIMETER, FRAME_DELIMETER_LEN);
          chd->sending = true;
        } else {
          /*
           * Frame may be followed by metadata, which is a comma-separated
           * list of values. Right now, only one field is epxected:
           * CRC32 checksum as a hex number.
           * TODO(rojer): Make it mandatory when updated mos has been out for
           * a while (today is 2017/03/28).
           */
          struct mg_str meta = mg_mk_str_n(f.p + f.len, 0);
          while (meta.p > f.p) {
            if (*(meta.p - 1) == '}') break;
            meta.p--;
            f.len--;
            meta.len++;
          }
          if (meta.len >= 8) {
            ((char *) meta.p)[meta.len] =
                '\0'; /* Stomps first char of the delimiter. */
            uint32_t crc = cs_crc32(0, f.p, f.len);
            uint32_t expected_crc = 0;
            if (sscanf(meta.p, "%x", (int *) &expected_crc) != 1 ||
                crc != expected_crc) {
              LOG(LL_WARN,
                  ("%p Corrupted frame (%d): '%.*s' '%.*s' %08x %08x", ch,
                   (int) f.len, (int) f.len, f.p, (int) meta.len, meta.p,
                   (unsigned int) expected_crc, (unsigned int) crc));
              f.len = 0;
            }
          }
          if (f.len > 0) {
            ch->ev_handler(ch, MG_RPC_CHANNEL_FRAME_RECD, &f);
          }
        }
      }
      mbuf_remove(urxb, flen + FRAME_DELIMETER_LEN);
    }
    if ((int) urxb->len >
        mgos_sys_config_get_rpc_max_frame_size() + 2 * FRAME_DELIMETER_LEN) {
      LOG(LL_ERROR, ("Incoming frame is too big, dropping."));
      mbuf_remove(urxb, urxb->len);
    }
    if (chd->waiting_for_start_frame && urxb->len > FRAME_DELIMETER_LEN) {
      mbuf_remove(urxb, urxb->len - FRAME_DELIMETER_LEN);
    }
    mbuf_trim(urxb);
  }
  size_t tx_av = mgos_uart_write_avail(uart_no);
  if (chd->sending && tx_av > 0) {
    size_t len = MIN(chd->send_mbuf.len, tx_av);
    len = mgos_uart_write(uart_no, chd->send_mbuf.buf, len);
    mbuf_remove(&chd->send_mbuf, len);
    if (chd->send_mbuf.len == 0) {
      chd->sending = false;
      if (chd->resume_uart) {
        chd->resume_uart = false;
        mgos_uart_flush(uart_no);
        mgos_debug_resume_uart();
      }
      if (chd->sending_user_frame) {
        chd->sending_user_frame = false;
        ch->ev_handler(ch, MG_RPC_CHANNEL_FRAME_SENT, (void *) 1);
      }
      mbuf_trim(&chd->send_mbuf);
    }
  }
}

static void mg_rpc_channel_uart_ch_connect(struct mg_rpc_channel *ch) {
  struct mg_rpc_channel_uart_data *chd =
      (struct mg_rpc_channel_uart_data *) ch->channel_data;
  if (!chd->connected) {
    chd->waiting_for_start_frame = chd->wait_for_start_frame;
    mgos_uart_set_dispatcher(chd->uart_no, mg_rpc_channel_uart_dispatcher, ch);
    mgos_uart_set_rx_enabled(chd->uart_no, true);
  }
}

static bool mg_rpc_channel_uart_send_frame(struct mg_rpc_channel *ch,
                                           const struct mg_str f) {
  struct mg_rpc_channel_uart_data *chd =
      (struct mg_rpc_channel_uart_data *) ch->channel_data;
  if (!chd->connected || chd->sending) return false;
  mbuf_append(&chd->send_mbuf, FRAME_DELIMETER, FRAME_DELIMETER_LEN);
  mbuf_append(&chd->send_mbuf, f.p, f.len);
  char crc_hex[9];
  sprintf(crc_hex, "%08x", (unsigned int) cs_crc32(0, f.p, f.len));
  mbuf_append(&chd->send_mbuf, crc_hex, 8);
  mbuf_append(&chd->send_mbuf, FRAME_DELIMETER, FRAME_DELIMETER_LEN);
  chd->sending = chd->sending_user_frame = true;

  /* Disable UART console while sending. */
  if (mgos_get_stdout_uart() == chd->uart_no ||
      mgos_get_stderr_uart() == chd->uart_no) {
    mgos_debug_suspend_uart();
    chd->resume_uart = true;
  } else {
    chd->resume_uart = false;
  }

  mgos_uart_schedule_dispatcher(chd->uart_no, false /* from_isr */);
  return true;
}

static void mg_rpc_channel_uart_ch_close(struct mg_rpc_channel *ch) {
  struct mg_rpc_channel_uart_data *chd =
      (struct mg_rpc_channel_uart_data *) ch->channel_data;
  mgos_uart_set_dispatcher(chd->uart_no, NULL, NULL);
  chd->connected = chd->sending = chd->sending_user_frame = false;
  if (chd->resume_uart) mgos_debug_resume_uart();
  ch->ev_handler(ch, MG_RPC_CHANNEL_CLOSED, NULL);
}

static void mg_rpc_channel_uart_ch_destroy(struct mg_rpc_channel *ch) {
  struct mg_rpc_channel_uart_data *chd =
      (struct mg_rpc_channel_uart_data *) ch->channel_data;
  mbuf_free(&chd->recv_mbuf);
  mbuf_free(&chd->send_mbuf);
  free(chd);
  free(ch);
}

static const char *mg_rpc_channel_uart_get_type(struct mg_rpc_channel *ch) {
  (void) ch;
  return "UART";
}

static bool mg_rpc_channel_uart_get_authn_info(
    struct mg_rpc_channel *ch, const char *auth_domain, const char *auth_file,
    struct mg_rpc_authn_info *authn) {
  (void) ch;
  (void) auth_domain;
  (void) auth_file;
  (void) authn;

  return false;
}

static char *mg_rpc_channel_uart_get_info(struct mg_rpc_channel *ch) {
  struct mg_rpc_channel_uart_data *chd =
      (struct mg_rpc_channel_uart_data *) ch->channel_data;
  char *res = NULL;
  mg_asprintf(&res, 0, "UART%d", chd->uart_no);
  return res;
}

struct mg_rpc_channel *mg_rpc_channel_uart(int uart_no,
                                           bool wait_for_start_frame) {
  struct mg_rpc_channel *ch = (struct mg_rpc_channel *) calloc(1, sizeof(*ch));
  ch->ch_connect = mg_rpc_channel_uart_ch_connect;
  ch->send_frame = mg_rpc_channel_uart_send_frame;
  ch->ch_close = mg_rpc_channel_uart_ch_close;
  ch->ch_destroy = mg_rpc_channel_uart_ch_destroy;
  ch->get_type = mg_rpc_channel_uart_get_type;
  ch->is_persistent = mg_rpc_channel_true;
  ch->is_broadcast_enabled = mg_rpc_channel_true;
  ch->get_authn_info = mg_rpc_channel_uart_get_authn_info;
  ch->get_info = mg_rpc_channel_uart_get_info;
  struct mg_rpc_channel_uart_data *chd =
      (struct mg_rpc_channel_uart_data *) calloc(1, sizeof(*chd));
  chd->uart_no = uart_no;
  chd->wait_for_start_frame = wait_for_start_frame;
  mbuf_init(&chd->recv_mbuf, 0);
  mbuf_init(&chd->send_mbuf, 0);
  ch->channel_data = chd;
  LOG(LL_INFO, ("%p UART%d", ch, uart_no));
  return ch;
}

bool mgos_rpc_uart_init(void) {
  const struct mgos_config_rpc *sccfg = mgos_sys_config_get_rpc();
  if (mgos_rpc_get_global() == NULL || sccfg->uart.uart_no < 0) return true;

  const struct mgos_config_rpc_uart *scucfg = mgos_sys_config_get_rpc_uart();
  struct mgos_uart_config ucfg;
  /* If UART is already configured (presumably for debug)
   * keep all the settings except maybe flow control */
  if (mgos_uart_config_get(scucfg->uart_no, &ucfg)) {
    mgos_uart_flush(scucfg->uart_no);
    ucfg.rx_fc_type = ucfg.tx_fc_type =
        (enum mgos_uart_fc_type) scucfg->fc_type;
  } else {
    mgos_uart_config_set_defaults(scucfg->uart_no, &ucfg);
    ucfg.baud_rate = scucfg->baud_rate;
    ucfg.rx_fc_type = ucfg.tx_fc_type =
        (enum mgos_uart_fc_type) scucfg->fc_type;
  }
  if (mgos_uart_configure(scucfg->uart_no, &ucfg)) {
    struct mg_rpc_channel *uch =
        mg_rpc_channel_uart(scucfg->uart_no, scucfg->wait_for_start_frame);
    mg_rpc_add_channel(mgos_rpc_get_global(), mg_mk_str(""), uch);
    uch->ch_connect(uch);
  } else {
    LOG(LL_ERROR, ("UART%d init failed", scucfg->uart_no));
    return false;
  }

  return true;
}
