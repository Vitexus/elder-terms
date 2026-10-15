import { spawnSync } from 'node:child_process';
import { mkdtemp, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';

const executable = fileURLToPath(
  new URL('../../.build/elder-terms-vte/boxrig-regex', import.meta.url)
);

const match = (args: string[], input: string | Buffer) => {
  const result = spawnSync(executable, args, {
    input,
    encoding: 'utf8',
    env: { PATH: process.env.PATH, LC_ALL: 'C.UTF-8' },
  });
  expect(result.error).toBeUndefined();
  expect(result.signal).toBeNull();
  return result;
};

describe('boxrig regex without a display or IME', () => {
  it('keeps Unicode host captures and byte positions with a work budget of one', () => {
    const pattern = String.raw`(?<scheme>https?)://(?<host>[\p{L}0-9.-]+)`;
    const input = '前 https://example.みんな/';
    const regular = match(['--captures', pattern], input);
    const incremental = match(['--captures', pattern, '0', '1'], input);
    expect(regular.status).toBe(0);
    expect(regular.stderr).toBe('');
    expect(regular.stdout).toMatch(
      /^match 4 29 work=\d+\ngroup 0 0 25\ngroup 1 0 5\ngroup 2 8 25\n$/
    );
    expect(incremental.status).toBe(0);
    expect(incremental.stdout).toBe(regular.stdout);
  });

  it('distinguishes an absent group from a participating empty group', () => {
    const result = match(['--captures', '(a)?(c*)(b)'], 'b');
    expect(result.status).toBe(0);
    expect(result.stdout).toMatch(
      /group 0 0 1\ngroup 1 absent\ngroup 2 0 0\ngroup 3 0 1\n$/
    );
  });

  it('selects the longest alternative at the leftmost position', () => {
    const result = match(['a|abc'], 'xabcabc');
    expect(result.status).toBe(0);
    expect(result.stdout).toMatch(/^match 1 4 work=\d+\n$/);
  });

  it('includes LF in dot and anchors to the whole input', () => {
    expect(match(['^a.b$'], 'a\nb').status).toBe(0);
    const result = match(['^b$'], 'a\nb');
    expect(result.status).toBe(1);
    expect(result.stdout).toMatch(/^not-found work=\d+\n$/);
  });

  it('requires grapheme boundaries without silently extending letter classes', () => {
    expect(match([String.raw`\p{L}+`], 'e\u0301').status).toBe(1);
    expect(match([String.raw`[\p{L}\p{M}]+`], 'e\u0301').status).toBe(0);
    expect(match(['a', '1', '1'], 'e\u0301a').stdout).toMatch(/^match 3 4 /);
    const invalid = match(['a', '2'], 'e\u0301a');
    expect(invalid.status).toBe(2);
    expect(invalid.stderr).toContain('input error at byte 2');
  });

  it('keeps ASCII digits separate from Unicode decimal digits', () => {
    expect(match([String.raw`\d+`], '١２').status).toBe(1);
    expect(match([String.raw`\p{Nd}+`], '١２').stdout).toMatch(/^match 0 5 /);
    expect(match([String.raw`\P{L}+`], '字١２').stdout).toMatch(/^match 3 8 /);
  });

  it('reports invalid categories at their UTF-8 byte offset and rejects empty matches', () => {
    const invalid = match([String.raw`日\p{Bogus}`], '日本');
    expect(invalid.status).toBe(2);
    expect(invalid.stderr).toContain('pattern error at byte 3 (code 2,');
    const empty = match(['a*'], 'abc');
    expect(empty.status).toBe(2);
    expect(empty.stderr).toContain('(code 7,');
  });

  it('accepts control characters and NUL in length-delimited input and patterns', async () => {
    expect(
      match([String.raw`\u{1b}\[31m(?<word>OK)`], '\x1b[31mOK').status
    ).toBe(0);
    const directory = await mkdtemp(join(tmpdir(), 'elder-boxrig-regex-'));
    try {
      const path = join(directory, 'pattern');
      await writeFile(path, 'a\0b');
      const result = match(['--pattern-file', path], 'xa\0by');
      expect(result.status).toBe(0);
      expect(result.stdout).toMatch(/^match 1 4 /);
    } finally {
      await rm(directory, { recursive: true, force: true });
    }
  });

  it('rejects invalid UTF-8 and input above the configured capacity', () => {
    const invalid = match(['a'], Buffer.from([0x61, 0xff]));
    expect(invalid.status).toBe(2);
    expect(invalid.stderr).toContain('input error at byte 1');
    const overflow = match(['a'], Buffer.alloc(1024 * 1024 + 1, 'a'));
    expect(overflow.status).toBe(2);
    expect(overflow.stderr).toContain(
      'Cannot read input within the configured byte capacity.'
    );
  });
});
