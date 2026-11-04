// SPDX-License-Identifier: GPL-3.0-or-later
// Usage: node scripts/generate-brand-icons.mjs [absolute path to sharp package]
// Sharp is a build-time SVG renderer; no application dependency is required.
import fs from 'node:fs/promises';
import path from 'node:path';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';

const require = createRequire(import.meta.url);
const sharp = require(process.argv[2] || 'sharp');
const output = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../assets/branding');
await fs.mkdir(output, { recursive: true });

// Aurora's approved native palette, translated from ribbon folds to gem facets.
const defs = `<defs>
    <linearGradient id="surface" x1="0" y1="0" x2="0" y2="1">
      <stop stop-color="#25262e"/><stop offset="1" stop-color="#060609"/>
    </linearGradient>
    <linearGradient id="gem" gradientUnits="userSpaceOnUse" x1="310" y1="250" x2="690" y2="780">
      <stop stop-color="#27e3d2"/><stop offset=".35" stop-color="#65dce8"/>
      <stop offset=".66" stop-color="#5d5ce8"/><stop offset="1" stop-color="#b17bfa"/>
    </linearGradient>
  </defs>`;

// Eight rays, each bisected into two triangular planes. No outlines or effects.
const points = [
  [512, 180], [566, 370], [730, 294], [654, 458],
  [844, 512], [654, 566], [730, 730], [566, 654],
  [512, 844], [458, 654], [294, 730], [370, 566],
  [180, 512], [370, 458], [294, 294], [458, 370],
];
const overlays = [
  ['#d8ffff', .34], ['#1244a5', .08], ['#d8ffff', .15], ['#1244a5', .19],
  ['#d8ffff', .16], ['#1244a5', .25], ['#d8ffff', .12], ['#563cb6', .26],
  ['#d8ffff', .13], ['#1244a5', .28], ['#d8ffff', .12], ['#1244a5', .25],
  ['#d8ffff', .14], ['#138aab', .24], ['#d8ffff', .26], ['#138aab', .16],
];
const silhouette = `M${points.map(p => p.join(' ')).join(' L')}Z`;
const facets = points.map((p, i) => {
  const next = points[(i + 1) % points.length];
  const [color, opacity] = overlays[i];
  return `    <path d="M512 512 L${p.join(' ')} L${next.join(' ')}Z" fill="${color}" fill-opacity="${opacity}"/>`;
}).join('\n');
const mark = `<g>
    <path d="${silhouette}" fill="url(#gem)"/>
${facets}
  </g>`;

// Match Aurora's n=5 superellipse, including the 40 px transparent outer inset.
const squircle = Array.from({ length: 256 }, (_, i) => {
  const t = i / 256 * 2 * Math.PI;
  return [Math.cos(t), Math.sin(t)].map(v => (512 + Math.sign(v) * Math.abs(v) ** .4 * 472).toFixed(2)).join(' ');
});
const wrap = (content, label) => `<svg xmlns="http://www.w3.org/2000/svg" width="1024" height="1024" viewBox="0 0 1024 1024" role="img" aria-labelledby="title desc">
  <title id="title">${label}</title>
  <desc id="desc">An eight-point geometric starburst with teal, cyan, indigo and violet gem facets.</desc>
  ${defs}
  ${content}
</svg>\n`;
const app = wrap(`<path d="M${squircle.join(' L')}Z" fill="url(#surface)"/>\n  ${mark}`, 'Nova Studio app icon');
const standalone = wrap(mark, 'Nova Studio starburst mark');
for (const [name, svg] of [['nova-studio-icon', app], ['nova-studio-mark', standalone]]) {
  await fs.writeFile(path.join(output, `${name}.svg`), svg);
  await sharp(Buffer.from(svg), { density: 288 }).resize(1024, 1024).png().toFile(path.join(output, `${name}.png`));
}
const frames = [];
const sizes = [16, 24, 32, 48, 64, 128, 256];
for (const size of [...sizes, 512]) {
  const frame = await sharp(Buffer.from(app), { density: 288 }).resize(size, size).png().toBuffer();
  await fs.writeFile(path.join(output, `nova-studio-icon-${size}.png`), frame);
  if (size <= 256) frames.push(frame);
}
const header = Buffer.alloc(6 + 16 * frames.length);
header.writeUInt16LE(1, 2);
header.writeUInt16LE(frames.length, 4);
let offset = header.length;
frames.forEach((frame, i) => {
  const at = 6 + i * 16;
  header[at] = sizes[i] % 256;
  header[at + 1] = sizes[i] % 256;
  header.writeUInt16LE(1, at + 4);
  header.writeUInt16LE(32, at + 6);
  header.writeUInt32LE(frame.length, at + 8);
  header.writeUInt32LE(offset, at + 12);
  offset += frame.length;
});
await fs.writeFile(path.join(output, 'nova-studio-icon.ico'), Buffer.concat([header, ...frames]));
console.log(`Generated SVG masters, 1024 px PNGs and application sizes in ${output}`);
