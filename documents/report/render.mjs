// Renders the report to PDF with headless Chromium.
//   node render.mjs   -> ../Protosim_Protoduino_Functional_Technical_Report.pdf
// Needs the "playwright" package and the Inter and DejaVu Sans Mono fonts.
import { readFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { createRequire } from 'node:module';

const here = dirname(fileURLToPath(import.meta.url));
const { chromium } = createRequire(import.meta.url)('playwright');

const css = await readFile(join(here, 'doc.css'), 'utf8');
const files = ['part1.html', 'part2.html', 'part3.html', 'part4.html'];
const parts = await Promise.all(files.map((f) => readFile(join(here, f), 'utf8')));
const html = `<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<title>Protosim + Protoduino - functional and technical decision report</title>
<style>${css}</style></head>
<body>${parts.join('\n')}</body></html>`;

const out = join(here, '..', 'Protosim_Protoduino_Functional_Technical_Report.pdf');
const footer = `<div style="width:100%; font-family:Inter,sans-serif; font-size:7.5px; color:#5b6672; padding:0 17mm; display:flex; justify-content:space-between;">
  <span>Protosim + Protoduino &middot; engineering decision report &middot; v2.1 r4</span>
  <span><span class="pageNumber"></span> / <span class="totalPages"></span></span></div>`;

const browser = await chromium.launch();
const page = await browser.newPage();
await page.setContent(html, { waitUntil: 'load' });
await page.evaluate(() => document.fonts.ready);
await page.pdf({
  path: out,
  format: 'A4',
  printBackground: true,
  displayHeaderFooter: true,
  headerTemplate: '<span></span>',
  footerTemplate: footer,
  margin: { top: '19mm', bottom: '21mm', left: '17mm', right: '17mm' },
});
await browser.close();
console.log('wrote', out);
