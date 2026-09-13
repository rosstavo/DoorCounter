/*
 * serial-bridge.js — Vite dev-server plugin: ESP32 serial port <-> WebSocket.
 *
 * The tuner firmware speaks newline-delimited JSON over USB serial. Browsers
 * can't open a serial port (short of Chrome-only Web Serial), so the dev server
 * holds the port and relays it:
 *
 *   ESP32  --USB-->  Vite dev server  --ws://.../serial-->  browser(s)
 *
 * Every connected client gets every line, and anything a client sends is
 * written straight back to the device. Multiple clients are fine and useful:
 * keep the laptop plugged into the board at the doorway and open the same URL
 * on your phone to read the traces while you walk through.
 *
 * The port is reopened automatically if the board is reset or replugged (this
 * board's USB port likes to re-enumerate, e.g. -2110 -> -1110), so a reflash
 * mid-session doesn't need the dev server restarted.
 *
 * Override the port with:  PORT=/dev/cu.usbserial-XXXX npm run dev
 */

import { WebSocketServer } from 'ws';
import { SerialPort, ReadlineParser } from 'serialport';
import { existsSync } from 'node:fs';

const BAUD = 115200;
const RECONNECT_MS = 2000;

// USB-serial bridges we might plausibly be plugged into. The Olimex board shows
// up as cu.usbserial-*; the others cover common CP210x/CH340/native-USB boards.
const PORT_PATTERNS = [/usbserial/i, /usbmodem/i, /SLAB_USBtoUART/i, /wchusbserial/i];

async function findPort() {
  if (process.env.PORT) return process.env.PORT;
  const ports = await SerialPort.list();
  const match = ports
    .map((p) => p.path)
    .filter((path) => PORT_PATTERNS.some((re) => re.test(path)))
    .sort();
  return match[0] ? preferCalloutDevice(match[0]) : null;
}

// macOS exposes each USB serial device twice, but SerialPort.list() only ever
// reports the /dev/tty.* "dial-in" node — which blocks on carrier detect and can
// hang the open. The /dev/cu.* "callout" twin is the one to use (and the one the
// repo's other scripts use), so swap to it whenever it exists.
function preferCalloutDevice(path) {
  const callout = path.replace('/dev/tty.', '/dev/cu.');
  return callout !== path && existsSync(callout) ? callout : path;
}

export function serialBridge() {
  return {
    name: 'esp32-serial-bridge',
    configureServer(server) {
      const wss = new WebSocketServer({ noServer: true });
      let port = null;
      let currentPath = null;
      let reconnectTimer = null;
      let closed = false;

      const broadcast = (text) => {
        for (const client of wss.clients) {
          if (client.readyState === 1) client.send(text);
        }
      };

      // Bridge-level status, distinguished from device messages by t:"bridge".
      const announce = (state, detail) => {
        broadcast(
          JSON.stringify({ t: 'bridge', state, port: currentPath, detail: detail || '' })
        );
      };

      const scheduleReconnect = () => {
        if (closed || reconnectTimer) return;
        reconnectTimer = setTimeout(() => {
          reconnectTimer = null;
          openPort();
        }, RECONNECT_MS);
      };

      const openPort = async () => {
        if (closed || port) return;
        let path;
        try {
          path = await findPort();
        } catch (err) {
          announce('error', `could not list serial ports: ${err.message}`);
          return scheduleReconnect();
        }
        if (!path) {
          currentPath = null;
          announce('searching', 'no USB serial port found — is the board plugged in?');
          return scheduleReconnect();
        }

        currentPath = path;
        const p = new SerialPort({ path, baudRate: BAUD, autoOpen: false });

        p.open((err) => {
          if (err) {
            // Commonly "Resource busy" — a serial monitor still holds the port.
            announce('error', `${path}: ${err.message}`);
            port = null;
            return scheduleReconnect();
          }
          port = p;
          server.config.logger.info(`  ➜  Serial:  ${path} @ ${BAUD}`);
          announce('open');
        });

        p.pipe(new ReadlineParser({ delimiter: '\n' })).on('data', (line) => {
          const text = line.trim();
          if (text) broadcast(text);
        });

        p.on('close', () => {
          if (port === p) port = null;
          announce('closed', 'device disconnected');
          scheduleReconnect();
        });
        p.on('error', (err) => {
          if (port === p) port = null;
          announce('error', err.message);
          scheduleReconnect();
        });
      };

      // Vite runs its own HMR socket on this server, so claim only /serial and
      // let every other upgrade fall through to Vite untouched.
      server.httpServer?.on('upgrade', (req, socket, head) => {
        if (!req.url || !req.url.startsWith('/serial')) return;
        wss.handleUpgrade(req, socket, head, (ws) => wss.emit('connection', ws, req));
      });

      wss.on('connection', (ws) => {
        ws.send(
          JSON.stringify({
            t: 'bridge',
            state: port ? 'open' : 'closed',
            port: currentPath,
            detail: '',
          })
        );
        // Ask the device to restate its config, so a client that joins
        // mid-session isn't showing stale slider positions.
        if (port) port.write('get\n');

        ws.on('message', (data) => {
          const cmd = data.toString().trim();
          if (!cmd) return;
          if (!port) {
            ws.send(
              JSON.stringify({ t: 'bridge', state: 'closed', detail: 'no serial port open' })
            );
            return;
          }
          port.write(cmd + '\n');
        });
      });

      openPort();

      server.httpServer?.on('close', () => {
        closed = true;
        clearTimeout(reconnectTimer);
        port?.close(() => {});
        wss.close();
      });
    },
  };
}
