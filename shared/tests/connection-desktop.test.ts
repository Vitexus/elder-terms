import { execFile } from 'node:child_process';
import { mkdtemp, mkdir, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';
import { PNG } from 'pngjs';
import { expect, it } from 'vitest';

const exec = promisify(execFile);
const driver = fileURLToPath(
  new URL('../../.build/shared/connection-desktop-driver', import.meta.url)
);
const id = 'f724d380-7b23-49c3-8a5b-4213723af440';

it('publishes distinct images and preserves literal desktop command arguments', async () => {
  const directory = await mkdtemp(join(tmpdir(), 'elder desktop '));
  const env = { ...process.env, XDG_DATA_HOME: directory };
  try {
    const args = [
      'spaces here',
      '日本語',
      'a"b',
      "single'quote",
      '$HOME',
      '`echo nope`',
      'a\\b',
      '100% %f',
      'line\nbreak',
      '',
    ];
    const command = [
      process.execPath,
      '-e',
      'process.stdout.write(JSON.stringify(process.argv.slice(1)))',
      ...args,
    ];
    const created = await exec(
      driver,
      [id, '日本語 "name" %', 'ff0000ff', ...command],
      { env }
    );
    const [desktop, firstIcon] = created.stdout.trimEnd().split('\n');
    const launched = await exec('gio', ['launch', desktop], { env });
    expect(JSON.parse(launched.stdout)).toEqual(args);
    const first = PNG.sync.read(await readFile(firstIcon));
    expect([
      ...first.data.subarray(
        (128 * first.width + 128) * 4,
        (128 * first.width + 128) * 4 + 4
      ),
    ]).toEqual([255, 0, 0, 255]);
    const updated = await exec(
      driver,
      [id, 'Renamed', '0000ffff', ...command],
      { env }
    );
    const [sameDesktop, secondIcon] = updated.stdout.trimEnd().split('\n');
    expect(sameDesktop).toBe(desktop);
    expect(secondIcon).not.toBe(firstIcon);
    expect(await readFile(desktop, 'utf8')).toContain('Name=Renamed');
    expect(await readFile(desktop, 'utf8')).toContain('NoDisplay=true');
    const second = PNG.sync.read(await readFile(secondIcon));
    expect([
      ...second.data.subarray(
        (128 * second.width + 128) * 4,
        (128 * second.width + 128) * 4 + 4
      ),
    ]).toEqual([0, 0, 255, 255]);
    expect(PNG.sync.read(await readFile(firstIcon)).data).toEqual(first.data);
  } finally {
    await rm(directory, { recursive: true, force: true });
  }
});

it('leaves a user-created desktop entry intact', async () => {
  const directory = await mkdtemp(join(tmpdir(), 'elder-desktop-user-'));
  try {
    await mkdir(join(directory, 'applications'));
    const desktop = join(
      directory,
      'applications',
      `net.kekyo.elder-terms.connection.${id}.desktop`
    );
    const original =
      '[Desktop Entry]\nType=Application\nName=User entry\nExec=true\n';
    await writeFile(desktop, original);
    await expect(
      exec(driver, [id, 'Connection', 'ff0000ff', 'true'], {
        env: { ...process.env, XDG_DATA_HOME: directory },
      })
    ).rejects.toThrow('not managed by elder-terms');
    expect(await readFile(desktop, 'utf8')).toBe(original);
  } finally {
    await rm(directory, { recursive: true, force: true });
  }
});
