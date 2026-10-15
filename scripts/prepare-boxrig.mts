import { spawn } from 'node:child_process';
import { createHash } from 'node:crypto';
import {
  access,
  copyFile,
  lstat,
  mkdir,
  readFile,
  readdir,
  readlink,
  rm,
  symlink,
  writeFile,
} from 'node:fs/promises';
import { availableParallelism } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = fileURLToPath(new URL('..', import.meta.url));
const upstream = join(root, 'deps/boxrig');
const build = resolve(process.argv[2] ?? join(root, '.build'), 'boxrig');
const sdk = join(build, 'sdk');
const stamp = join(dirname(build), 'boxrig-sdk.stamp');

const exists = async (path: string): Promise<boolean> => {
  try {
    await access(path);
    return true;
  } catch {
    return false;
  }
};

const run = async (command: string, args: string[]): Promise<void> => {
  const child = spawn(command, args, { cwd: upstream, stdio: 'inherit' });
  const code = await new Promise<number | null>((resolveExit, reject) => {
    child.once('error', reject);
    child.once('exit', resolveExit);
  });
  if (code !== 0) throw new Error(`${command} exited with ${code}`);
};

const files = async (directory: string): Promise<string[]> => {
  const result: string[] = [];
  for (const entry of await readdir(directory, { withFileTypes: true })) {
    const path = join(directory, entry.name);
    if (entry.isDirectory()) result.push(...(await files(path)));
    else result.push(path);
  }
  return result.sort();
};

const copyChanged = async (
  source: string,
  destination: string
): Promise<void> => {
  const information = await lstat(source);
  if (information.isDirectory()) {
    await mkdir(destination, { recursive: true });
    const entries = await readdir(source);
    for (const entry of await readdir(destination)) {
      if (!entries.includes(entry))
        await rm(join(destination, entry), { recursive: true, force: true });
    }
    for (const entry of entries)
      await copyChanged(join(source, entry), join(destination, entry));
  } else if (information.isSymbolicLink()) {
    const target = await readlink(source);
    if ((await exists(destination)) && (await readlink(destination)) === target)
      return;
    await rm(destination, { force: true });
    await symlink(target, destination);
  } else if (
    !(await exists(destination)) ||
    !(await readFile(source)).equals(await readFile(destination))
  ) {
    await copyFile(source, destination);
  }
};

for (const required of [
  'Makefile',
  'deps/utf8proc/utf8proc.c',
  'deps/libpng/png.c',
  'deps/zlib/zlib.h',
]) {
  if (!(await exists(join(upstream, required)))) {
    throw new Error(
      'Initialize boxrig with git submodule update --init --recursive'
    );
  }
}
await mkdir(build, { recursive: true });
const lock = await readFile(join(upstream, 'tools/codegen/package-lock.json'));
const lockStamp = join(build, 'codegen-lock.json');
if (
  !(await exists(
    join(upstream, 'tools/codegen/node_modules/.package-lock.json')
  )) ||
  !(await exists(lockStamp)) ||
  !lock.equals(await readFile(lockStamp))
) {
  await run('npm', ['ci', '--prefix', 'tools/codegen']);
  await writeFile(lockStamp, lock);
}

// The gitlink records the exact source; pkg-config carries its release version.
const makeArgs = [
  `-j${Math.min(availableParallelism(), 8)}`,
  `BUILD_DIR=${build}`,
  'GTK_OPENGL=1',
  'BOXRIG_VERSION=0.1.0',
  `PREFIX=${sdk}`,
];
await run('make', [...makeArgs, 'shared-gtk3']);
const inputs = [
  ...(await files(join(upstream, 'include'))),
  ...(await files(join(upstream, 'packaging/licenses'))),
  ...(await files(join(build, 'shared/pkgconfig'))),
  ...[
    'boxrig',
    'boxrig-ui',
    'boxrig-session',
    'boxrig-posix',
    'boxrig-gtk3',
  ].flatMap((name) =>
    ['a', 'so.0'].map((extension) =>
      join(build, `shared/lib${name}.${extension}`)
    )
  ),
  ...[
    'LICENSE',
    'deps/utf8proc/LICENSE.md',
    'deps/libpng/LICENSE',
    'deps/zlib/LICENSE',
    'resources/unicode/17.0.0/LICENSE.txt',
    'data/encoding/whatwg/LICENSE',
    'examples/gtk3/external_scrollbar.h',
    'packaging/linux/rules.mk',
  ].map((path) => join(upstream, path)),
];
const digest = createHash('sha256');
for (const input of inputs) digest.update(input).update(await readFile(input));
const signature = digest.digest('hex');
if (
  !(await exists(stamp)) ||
  (await readFile(stamp, 'utf8')) !== signature ||
  !(await exists(join(sdk, 'lib/libboxrig-gtk3.so')))
) {
  const temporary = join(build, 'install-root');
  await rm(temporary, { recursive: true, force: true });
  await run('make', [
    ...makeArgs,
    `DESTDIR=${temporary}`,
    'install-shared-gtk3',
  ]);
  await copyChanged(join(temporary, sdk), sdk);
  await rm(temporary, { recursive: true, force: true });
  await writeFile(stamp, signature);
}
