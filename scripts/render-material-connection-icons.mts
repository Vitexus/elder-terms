import { spawn } from 'node:child_process';
import { mkdir, readdir } from 'node:fs/promises';
import { join } from 'node:path';

const [sourceRoot, resourceXml, outputSource, renderedRoot, convertCommand, glibCommand] = process.argv.slice(2);
if (!sourceRoot || !resourceXml || !outputSource || !renderedRoot || !convertCommand || !glibCommand) {
  throw new Error('Usage: node scripts/render-material-connection-icons.mts SVG_ROOT RESOURCE_XML OUTPUT_C RENDERED_ROOT CONVERT GLIB_COMPILE_RESOURCES');
}

const run = async (command: string, args: string[]): Promise<void> => {
  await new Promise<void>((resolve, reject) => {
    const child = spawn(command, args, { stdio: 'pipe' });
    let stderr = '';
    child.stderr.setEncoding('utf8');
    child.stderr.on('data', (chunk: string) => { stderr += chunk; });
    child.on('error', reject);
    child.on('close', (code) => code === 0 ? resolve() : reject(new Error(stderr)));
  });
};

const jobs: { source: string; destination: string }[] = [];
for (const category of (await readdir(sourceRoot)).sort()) {
  const inputDirectory = join(sourceRoot, category);
  const outputDirectory = join(renderedRoot, 'material', category);
  await mkdir(outputDirectory, { recursive: true });
  for (const filename of (await readdir(inputDirectory)).filter((name) => name.endsWith('.svg')).sort()) {
    jobs.push({
      source: join(inputDirectory, filename),
      destination: join(outputDirectory, `${filename.slice(0, -4)}.png`),
    });
  }
}
let nextJob = 0;
await Promise.all(Array.from({ length: 12 }, async () => {
  while (nextJob < jobs.length) {
    const job = jobs[nextJob++];
    await run(convertCommand, [
      '-size', '256x256', 'xc:none', '-fill', '#e6e8eb',
      '-draw', 'roundrectangle 0,0 255,255 32,32',
      '(', '+size', '-background', 'none', '-density', '704', job.source, ')',
      '-gravity', 'center', '-compose', 'over', '-composite', '-depth', '8',
      job.destination,
    ]);
  }
}));
await run(glibCommand, [
  '--generate-source', '--c-name', 'elder_terms_material_presets',
  '--sourcedir', renderedRoot, '--target', outputSource, resourceXml,
]);
