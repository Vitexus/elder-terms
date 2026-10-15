import { spawnSync } from 'node:child_process';
import {
  chmodSync,
  mkdirSync,
  mkdtempSync,
  rmSync,
  writeFileSync,
} from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { expect, it } from 'vitest';

const project = fileURLToPath(new URL('../..', import.meta.url));

it('rejects the retired bookworm package target', () => {
  const result = spawnSync(
    join(project, 'build_package.sh'),
    ['--version', '1.2.3', '--release', 'bookworm', '--print-version'],
    { encoding: 'utf8' }
  );
  expect(result.status, result.stdout).toBe(1);
  expect(result.stderr).toContain('Unsupported release filter: bookworm');
});

it('does not expose the retired terminal command in an installed command search', () => {
  const directory = mkdtempSync(join(tmpdir(), 'elder-retired-command-'));
  try {
    const introspection = spawnSync(
      'meson',
      ['introspect', '--buildoptions', join(project, '.build')],
      { encoding: 'utf8' }
    );
    expect(introspection.status, introspection.stderr).toBe(0);
    const options = JSON.parse(introspection.stdout) as {
      name: string;
      value: string;
    }[];
    const prefix = options.find((option) => option.name === 'prefix')!.value;
    const installation = spawnSync(
      'meson',
      [
        'install',
        '-C',
        join(project, '.build'),
        '--no-rebuild',
        '--destdir',
        directory,
      ],
      { encoding: 'utf8' }
    );
    expect(installation.status, installation.stderr).toBe(0);
    const lookup = spawnSync('/bin/sh', ['-c', 'command -v elder-terms-vte'], {
      env: { ...process.env, PATH: join(directory, prefix, 'bin') },
      encoding: 'utf8',
    });
    expect(lookup.status, lookup.stdout).not.toBe(0);
    expect(lookup.stdout).toBe('');
  } finally {
    rmSync(directory, { recursive: true, force: true });
  }
});

it('configures without VTE or a direct PCRE2 development dependency', () => {
  const directory = mkdtempSync(join(tmpdir(), 'elder-retired-dependencies-'));
  try {
    const bin = join(directory, 'bin');
    mkdirSync(bin);
    const pkgConfig = join(bin, 'pkg-config');
    writeFileSync(
      pkgConfig,
      `#!/bin/sh
for argument in "$@"; do
  case "$argument" in vte-2.91|libpcre2-8) exit 1 ;; esac
done
exec /usr/bin/pkg-config "$@"
`
    );
    chmodSync(pkgConfig, 0o755);
    const setup = spawnSync(
      'meson',
      ['setup', join(directory, 'build'), project, '-Dbuild_tests=false'],
      {
        env: {
          ...process.env,
          PATH: `${bin}:${process.env.PATH}`,
          PKG_CONFIG: pkgConfig,
        },
        encoding: 'utf8',
        maxBuffer: 8 * 1024 * 1024,
      }
    );
    expect(setup.status, `${setup.stdout}\n${setup.stderr}`).toBe(0);
  } finally {
    rmSync(directory, { recursive: true, force: true });
  }
}, 120_000);
