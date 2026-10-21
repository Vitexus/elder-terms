import { spawn } from 'node:child_process';
import { mkdir, readdir, rm } from 'node:fs/promises';
import { join } from 'node:path';

const [sourceRoot, resourceXml, outputSource, renderedRoot, convertCommand, glibCommand, thumbnailCommand] = process.argv.slice(2);
if (!sourceRoot || !resourceXml || !outputSource || !renderedRoot || !convertCommand || !glibCommand || !thumbnailCommand) {
  throw new Error('Usage: node scripts/render-material-connection-icons.mts SVG_ROOT RESOURCE_XML OUTPUT_C RENDERED_ROOT CONVERT GLIB_COMPILE_RESOURCES GDK_PIXBUF_THUMBNAILER');
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

const jobs: { source: string; black: string; white: string }[] = [];
for (const category of (await readdir(sourceRoot)).sort()) {
  const inputDirectory = join(sourceRoot, category);
  const outputDirectory = join(renderedRoot, 'material', category);
  const whiteDirectory = join(renderedRoot, 'material-white', category);
  await mkdir(outputDirectory, { recursive: true });
  await mkdir(whiteDirectory, { recursive: true });
  for (const filename of (await readdir(inputDirectory)).filter((name) => name.endsWith('.svg')).sort()) {
    jobs.push({
      source: join(inputDirectory, filename),
      black: join(outputDirectory, `${filename.slice(0, -4)}.png`),
      white: join(whiteDirectory, `${filename.slice(0, -4)}.png`),
    });
  }
}
let nextJob = 0;
await Promise.all(Array.from({ length: 12 }, async () => {
  while (nextJob < jobs.length) {
    const job = jobs[nextJob++];
    const raster = `${job.black}.raster.png`;
    try {
      // Use the SVG loader for compact arc commands unsupported by ImageMagick's
      // internal SVG parser. Keep the existing 176px artwork within a 256px canvas.
      await run(thumbnailCommand, ['-s', '176', job.source, raster]);
      await run(convertCommand, [
        '-background', 'none', raster,
        '-gravity', 'center', '-extent', '256x256',
        '-alpha', 'extract', '-write', 'mpr:glyph',
        // Dilate the completed silhouette, including hole boundaries, instead
        // of stroking SVG paths that may include invisible bounding rectangles.
        '-virtual-pixel', 'black', '-morphology', 'Dilate', 'Disk:8',
        '-write', 'mpr:outline', '-background', 'white', '-alpha', 'shape',
        '(', 'mpr:glyph', '-background', 'black', '-alpha', 'shape', ')',
        '-compose', 'Over', '-composite', '-depth', '8',
        '-write', `PNG32:${job.black}`, '+delete',
        'mpr:outline', '-background', 'black', '-alpha', 'shape',
        '(', 'mpr:glyph', '-background', 'white', '-alpha', 'shape', ')',
        '-compose', 'Over', '-composite', '-depth', '8', `PNG32:${job.white}`,
      ]);
    } catch (error) {
      throw new Error(`Could not render Material icon ${job.source}`, { cause: error });
    } finally {
      await rm(raster, { force: true });
    }
  }
}));
await run(glibCommand, [
  '--generate-source', '--c-name', 'elder_terms_material_presets',
  '--sourcedir', renderedRoot, '--target', outputSource, resourceXml,
]);
