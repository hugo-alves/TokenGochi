#!/usr/bin/env node
// Capture the current TokenGochi AMOLED frame over USB serial and write a PNG.
//
// Requires firmware with the TGSHOT serial command enabled. The firmware keeps
// a PSRAM-backed display mirror and streams it as raw RGB565 bytes.

import { execFileSync } from "node:child_process";
import {
  closeSync,
  constants as fsConstants,
  copyFileSync,
  existsSync,
  mkdirSync,
  openSync,
  readdirSync,
  readSync,
  writeFileSync,
  writeSync,
} from "node:fs";
import { basename, dirname, resolve } from "node:path";
import { deflateSync } from "node:zlib";

const DEFAULT_BAUD = 115200;
const DEFAULT_TIMEOUT_MS = 90000;
const DEFAULT_DIR = "screenshots";
const DEFAULT_MASK = "circle";
const DISPLAY_GEOMETRY = {
  width: 466,
  height: 466,
  centerX: 233,
  centerY: 233,
  radius: 233,
};

function usage() {
  console.log(`Usage:
  node tools/capture-device-screen.mjs [options]

Options:
  --port PATH          Serial port. Default: auto-detect USB serial
  --baud N            Serial baud rate. Default: ${DEFAULT_BAUD}
  --out FILE          PNG output path. Default: screenshots/tokengochi-<timestamp>.png
  --latest FILE       Also update this PNG copy. Default: screenshots/latest.png
  --no-latest         Do not update screenshots/latest.png
  --mask MODE         Pixel mask: circle or none. Default: ${DEFAULT_MASK}
  --timeout-ms N      Overall capture timeout. Default: ${DEFAULT_TIMEOUT_MS}
  --list-ports        Print candidate serial ports and exit
  -h, --help          Show this help

Examples:
  node tools/capture-device-screen.mjs
  node tools/capture-device-screen.mjs --out screenshots/color-check.png
  node tools/capture-device-screen.mjs --port /dev/cu.usbmodem1101
`);
}

function parseArgs(argv) {
  const opts = {
    baud: DEFAULT_BAUD,
    timeoutMs: DEFAULT_TIMEOUT_MS,
    latest: resolve(DEFAULT_DIR, "latest.png"),
    updateLatest: true,
    mask: DEFAULT_MASK,
  };

  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    const next = () => {
      if (i + 1 >= argv.length) throw new Error(`${a} requires a value`);
      return argv[++i];
    };

    switch (a) {
      case "-h":
      case "--help":
        opts.help = true;
        break;
      case "--list-ports":
        opts.listPorts = true;
        break;
      case "--port":
        opts.port = next();
        break;
      case "--baud":
        opts.baud = Number(next());
        break;
      case "--out":
        opts.out = resolve(next());
        break;
      case "--latest":
        opts.latest = resolve(next());
        opts.updateLatest = true;
        break;
      case "--no-latest":
        opts.updateLatest = false;
        break;
      case "--mask":
        opts.mask = next();
        break;
      case "--timeout-ms":
        opts.timeoutMs = Number(next());
        break;
      default:
        throw new Error(`unknown option: ${a}`);
    }
  }

  if (!opts.out) {
    const stamp = new Date().toISOString().replace(/[:.]/g, "-");
    opts.out = resolve(DEFAULT_DIR, `tokengochi-${stamp}.png`);
  }
  if (!["circle", "none"].includes(opts.mask)) throw new Error("invalid --mask; expected circle or none");
  if (!Number.isFinite(opts.baud) || opts.baud <= 0) throw new Error("invalid --baud");
  if (!Number.isFinite(opts.timeoutMs) || opts.timeoutMs <= 0) throw new Error("invalid --timeout-ms");
  return opts;
}

function candidatePorts() {
  const patterns = process.platform === "darwin"
    ? [/^cu\.(usbmodem|usbserial|SLAB_USBtoUART)/]
    : [/^tty(ACM|USB)\d+$/];
  try {
    return readdirSync("/dev")
      .filter((name) => patterns.some((pattern) => pattern.test(name)))
      .map((name) => `/dev/${name}`)
      .sort((a, b) => {
        const au = basename(a).startsWith("cu.usbmodem") ? 0 : 1;
        const bu = basename(b).startsWith("cu.usbmodem") ? 0 : 1;
        return au - bu || a.localeCompare(b);
      });
  } catch {
    return [];
  }
}

function resolvePort(requested) {
  if (requested) return requested;
  if (process.env.TOKENGOCHI_PORT) return process.env.TOKENGOCHI_PORT;
  const ports = candidatePorts();
  if (ports.length > 0) return ports[0];
  return null;
}

function configureSerial(port, baud) {
  const flag = process.platform === "darwin" ? "-f" : "-F";
  execFileSync("stty", [
    flag,
    port,
    String(baud),
    "raw",
    "-echo",
    "-icanon",
    "clocal",
    "-hupcl",
    "min", "0",
    "time", "10",
  ]);
}

function openSerial(port) {
  return openSync(port, fsConstants.O_RDWR | fsConstants.O_NONBLOCK | (fsConstants.O_NOCTTY ?? 0));
}

function readSome(fd) {
  const buf = Buffer.alloc(4096);
  try {
    const n = readSync(fd, buf, 0, buf.length, null);
    return n > 0 ? buf.subarray(0, n) : Buffer.alloc(0);
  } catch (err) {
    if (err && (err.code === "EAGAIN" || err.code === "EWOULDBLOCK")) return Buffer.alloc(0);
    throw err;
  }
}

function sleepMs(ms) {
  Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, ms);
}

function readUntilHeader(fd, deadline) {
  let buf = Buffer.alloc(0);
  const marker = Buffer.from("TGSHOT BEGIN ");

  while (Date.now() < deadline) {
    const chunk = readSome(fd);
    if (!chunk.length) {
      sleepMs(10);
      continue;
    }

    buf = Buffer.concat([buf, chunk]);
    const start = buf.indexOf(marker);
    if (start >= 0) {
      const nl = buf.indexOf(0x0a, start);
      if (nl >= 0) {
        const line = buf.subarray(start, nl).toString("utf8").trim();
        const m = line.match(/^TGSHOT BEGIN (\d+) (\d+) (RGB565BE|RGB565LE) (\d+)$/);
        if (!m) throw new Error(`bad TGSHOT header: ${line}`);
        return {
          width: Number(m[1]),
          height: Number(m[2]),
          encoding: m[3],
          byteCount: Number(m[4]),
          pending: buf.subarray(nl + 1),
        };
      }
    }

    const err = buf.indexOf(Buffer.from("TGSHOT ERR "));
    if (err >= 0) {
      const nl = buf.indexOf(0x0a, err);
      if (nl >= 0) throw new Error(buf.subarray(err, nl).toString("utf8").trim());
    }

    if (buf.length > 64 * 1024) buf = buf.subarray(buf.length - 64 * 1024);
  }
  throw new Error("timed out waiting for TGSHOT header");
}

function requestHeader(fd, deadline) {
  let lastError = null;

  while (Date.now() < deadline) {
    writeSync(fd, Buffer.from("\nTGWAKE\n"));
    sleepMs(150);
    writeSync(fd, Buffer.from("\nTGSHOT\n"));
    try {
      return readUntilHeader(fd, Math.min(deadline, Date.now() + 3000));
    } catch (err) {
      lastError = err;
      if (!(err instanceof Error) || err.message !== "timed out waiting for TGSHOT header") {
        throw err;
      }
    }
  }

  throw lastError ?? new Error("timed out waiting for TGSHOT header");
}

function readExactPayload(fd, first, byteCount, deadline) {
  const parts = [];
  let got = 0;
  if (first.length) {
    const take = Math.min(first.length, byteCount);
    parts.push(first.subarray(0, take));
    got += take;
  }

  while (got < byteCount && Date.now() < deadline) {
    const chunk = readSome(fd);
    if (!chunk.length) {
      sleepMs(10);
      continue;
    }
    const take = Math.min(chunk.length, byteCount - got);
    parts.push(chunk.subarray(0, take));
    got += take;
  }

  if (got !== byteCount) throw new Error(`short TGSHOT payload: got ${got}, expected ${byteCount}`);
  return Buffer.concat(parts, byteCount);
}

const crcTable = new Uint32Array(256);
for (let n = 0; n < 256; n++) {
  let c = n;
  for (let k = 0; k < 8; k++) c = (c & 1) ? (0xedb88320 ^ (c >>> 1)) : (c >>> 1);
  crcTable[n] = c >>> 0;
}

function crc32(buffers) {
  let c = 0xffffffff;
  for (const b of buffers) {
    for (const x of b) c = crcTable[(c ^ x) & 0xff] ^ (c >>> 8);
  }
  return (c ^ 0xffffffff) >>> 0;
}

function pngChunk(type, data = Buffer.alloc(0)) {
  const t = Buffer.from(type, "ascii");
  const out = Buffer.alloc(12 + data.length);
  out.writeUInt32BE(data.length, 0);
  t.copy(out, 4);
  data.copy(out, 8);
  out.writeUInt32BE(crc32([t, data]), 8 + data.length);
  return out;
}

function insideCircle(x, y, width, height) {
  const scaleX = width / DISPLAY_GEOMETRY.width;
  const scaleY = height / DISPLAY_GEOMETRY.height;
  const cx = DISPLAY_GEOMETRY.centerX * scaleX;
  const cy = DISPLAY_GEOMETRY.centerY * scaleY;
  const r = Math.min(DISPLAY_GEOMETRY.radius * scaleX, DISPLAY_GEOMETRY.radius * scaleY);
  return ((x - cx) ** 2 + (y - cy) ** 2) <= r ** 2;
}

function rgb565ToPng(raw, width, height, encoding, mask) {
  const expected = width * height * 2;
  if (raw.length !== expected) throw new Error(`bad raw length: got ${raw.length}, expected ${expected}`);

  const channels = mask === "circle" ? 4 : 3;
  const scanlines = Buffer.alloc((width * channels + 1) * height);
  let src = 0;
  let dst = 0;
  for (let y = 0; y < height; y++) {
    scanlines[dst++] = 0;
    for (let x = 0; x < width; x++) {
      const v = encoding === "RGB565LE"
        ? (raw[src] | (raw[src + 1] << 8))
        : ((raw[src] << 8) | raw[src + 1]);
      src += 2;
      scanlines[dst++] = Math.round((((v >> 11) & 0x1f) * 255) / 31);
      scanlines[dst++] = Math.round((((v >> 5) & 0x3f) * 255) / 63);
      scanlines[dst++] = Math.round(((v & 0x1f) * 255) / 31);
      if (channels === 4) scanlines[dst++] = insideCircle(x, y, width, height) ? 255 : 0;
    }
  }

  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(width, 0);
  ihdr.writeUInt32BE(height, 4);
  ihdr[8] = 8;
  ihdr[9] = channels === 4 ? 6 : 2;

  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    pngChunk("IHDR", ihdr),
    pngChunk("IDAT", deflateSync(scanlines)),
    pngChunk("IEND"),
  ]);
}

function capture(opts) {
  const port = resolvePort(opts.port);
  if (!port) {
    throw new Error("no serial port found. Connect the M5Stack or pass --port /dev/cu.usbmodem...");
  }
  if (!existsSync(port)) throw new Error(`serial port not found: ${port}`);

  const fd = openSerial(port);
  configureSerial(port, opts.baud);
  const deadline = Date.now() + opts.timeoutMs;

  try {
    while (Date.now() < deadline) {
      const chunk = readSome(fd);
      if (!chunk.length) break;
    }

    const header = requestHeader(fd, deadline);
    if (header.byteCount !== header.width * header.height * 2) {
      throw new Error(`unexpected byte count: ${header.byteCount} for ${header.width}x${header.height}`);
    }

    const raw = readExactPayload(fd, header.pending, header.byteCount, deadline);
    const png = rgb565ToPng(raw, header.width, header.height, header.encoding, opts.mask);

    mkdirSync(dirname(opts.out), { recursive: true });
    writeFileSync(opts.out, png);

    let latest = null;
    if (opts.updateLatest && opts.latest && resolve(opts.latest) !== resolve(opts.out)) {
      mkdirSync(dirname(opts.latest), { recursive: true });
      copyFileSync(opts.out, opts.latest);
      latest = opts.latest;
    }

    return { port, out: opts.out, latest, mask: opts.mask, ...header };
  } finally {
    closeSync(fd);
  }
}

try {
  const opts = parseArgs(process.argv.slice(2));
  if (opts.help) {
    usage();
    process.exit(0);
  }
  if (opts.listPorts) {
    const ports = candidatePorts();
    if (ports.length) console.log(ports.join("\n"));
    process.exit(0);
  }

  const result = capture(opts);
  console.log(`captured ${result.width}x${result.height} ${result.encoding} mask=${result.mask} from ${result.port}`);
  console.log(`wrote ${result.out}`);
  if (result.latest) console.log(`updated ${result.latest}`);
} catch (err) {
  console.error(`capture-device-screen: ${err instanceof Error ? err.message : String(err)}`);
  process.exit(1);
}
