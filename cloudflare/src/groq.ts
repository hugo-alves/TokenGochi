export interface TranscriptionResult {
  text: string;
  durationMs: number;
  lang: string | null;
  msGroq: number;
}

export function wavDurationMs(buf: ArrayBuffer): number | null {
  if (buf.byteLength < 44) return null;
  const view = new DataView(buf);
  const riff = String.fromCharCode(
    view.getUint8(0), view.getUint8(1), view.getUint8(2), view.getUint8(3)
  );
  const wave = String.fromCharCode(
    view.getUint8(8), view.getUint8(9), view.getUint8(10), view.getUint8(11)
  );
  if (riff !== "RIFF" || wave !== "WAVE") return null;

  let off = 12;
  while (off + 8 <= buf.byteLength) {
    const chunkId = String.fromCharCode(
      view.getUint8(off), view.getUint8(off + 1),
      view.getUint8(off + 2), view.getUint8(off + 3)
    );
    const chunkSize = view.getUint32(off + 4, true);
    const hdrEnd = off + 8 + chunkSize;
    if (chunkId === "fmt ") {
      const fmtOff = off + 8;
      const numChannels = view.getUint16(fmtOff + 2, true);
      const sampleRate = view.getUint32(fmtOff + 4, true);
      const byteRate = view.getUint32(fmtOff + 8, true);
      const bps = view.getUint16(fmtOff + 14, true);
      const bytesPerSec = byteRate || sampleRate * numChannels * (bps / 8);
      if (!Number.isFinite(bytesPerSec) || bytesPerSec <= 0) return null;
      let dataOff = off + 8 + chunkSize;
      while (dataOff + 8 <= view.byteLength) {
        const id = String.fromCharCode(
          view.getUint8(dataOff), view.getUint8(dataOff + 1),
          view.getUint8(dataOff + 2), view.getUint8(dataOff + 3)
        );
        const size = view.getUint32(dataOff + 4, true);
        if (id === "data") {
          return Math.round((size / bytesPerSec) * 1000);
        }
        dataOff += 8 + size;
      }
      return null;
    }
    off = hdrEnd;
  }
  return null;
}

export async function transcribeAudio(
  apiUrl: string,
  apiKey: string,
  model: string,
  wav: ArrayBuffer
): Promise<TranscriptionResult> {
  const startedAt = Date.now();
  const payload = new FormData();
  payload.append("file", new Blob([wav], { type: "audio/wav" }), "audio.wav");
  payload.append("model", model);
  payload.append("response_format", "json");

  const groqRes = await fetch(apiUrl, {
    method: "POST",
    headers: { Authorization: `Bearer ${apiKey}` },
    body: payload,
  });

  if (!groqRes.ok) {
    await groqRes.arrayBuffer();
    throw new Error(`groq ${groqRes.status}`);
  }

  const data = await groqRes.json() as { text?: unknown; language?: unknown };
  const text = typeof data.text === "string" ? data.text : "";
  const lang = typeof data.language === "string" ? data.language : null;
  return {
    text,
    lang,
    durationMs: wavDurationMs(wav) ?? 0,
    msGroq: Date.now() - startedAt,
  };
}
