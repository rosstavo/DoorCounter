import { defineConfig } from 'vite';
import { serialBridge } from './serial-bridge.js';

export default defineConfig({
  plugins: [serialBridge()],
  server: {
    // Bind to the LAN as well as localhost: the laptop stays tethered to the
    // ESP32 at the doorway while you read the traces on a phone.
    host: true,
    port: 5273,
  },
});
