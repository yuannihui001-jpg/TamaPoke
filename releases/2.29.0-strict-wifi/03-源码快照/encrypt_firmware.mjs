import { readFileSync, writeFileSync } from "node:fs";
import { randomBytes, createCipheriv } from "node:crypto";

const keyText = readFileSync(new URL("../cloudflare/FIRMWARE_KEY.local.txt", import.meta.url), "utf8").trim();
const key = Buffer.from(keyText.replace(/-/g, "+").replace(/_/g, "/") + "=".repeat((4 - (keyText.length % 4)) % 4), "base64");
if (key.length !== 32) throw new Error(`FIRMWARE_KEY must decode to 32 bytes, got ${key.length}`);

for (const [input, output] of process.argv.slice(2).reduce((pairs, path, index, args) => {
  if (index % 2 === 0) pairs.push([path, args[index + 1]]);
  return pairs;
}, [])) {
  const iv = randomBytes(12);
  const cipher = createCipheriv("aes-256-gcm", key, iv);
  const encrypted = Buffer.concat([cipher.update(readFileSync(input)), cipher.final(), cipher.getAuthTag()]);
  writeFileSync(output, Buffer.concat([iv, encrypted]));
  console.log(`${output}: ${encrypted.length + iv.length} bytes`);
}
