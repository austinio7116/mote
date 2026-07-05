/*
 * Mote OS — the standard multiplayer lobby (ABI v44, MoteApi.net_lobby).
 *
 * A blocking, engine-drawn flow a game invokes to get a connected opponent:
 * pick a transport, connect, and run the engine-owned nonce handshake. The game
 * then uses link_send/recv as normal. Rooms/links are gated by game id, so only
 * the same game (same protocol version) can ever pair — even over a USB cable.
 *
 * Step 2: USB transport (the device dual-role link / host socket). LAN + INTERNET
 * are shown once the device->Studio control protocol lands (step 3).
 */
#ifndef MOTE_LOBBY_H
#define MOTE_LOBBY_H

#include "mote_api.h"   /* MoteNetCfg, MOTE_NET_* */

int mote_lobby(const MoteNetCfg *cfg, int *out_is_host);

#endif /* MOTE_LOBBY_H */
