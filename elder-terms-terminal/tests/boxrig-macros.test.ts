import { fileURLToPath } from 'node:url';
import { createGtkAppLauncher } from 'gestament';
import { toPass } from 'gestament/testing';
import { describe, expect, it } from 'vitest';

const appPath = fileURLToPath(
  new URL('../../.build/elder-terms-terminal/boxrig-driver', import.meta.url)
);

describe('boxrig received macros', () => {
  for (const backend of ['opengl', 'cairo'] as const) {
    it(`sends captures to a PTY and observes command arguments with ${backend}`, async () => {
      const launcher = createGtkAppLauncher({
        appPath,
        env: {
          LIBGL_ALWAYS_SOFTWARE: '1',
          GTK_IM_MODULE: 'gtk-im-context-simple',
          ...(backend === 'cairo' ? { GDK_GL: 'disable' } : {}),
        },
      });
      try {
        const app = await launcher.launch([
          '--macro-send',
          'READY (?<name>[\\p{L}]+)',
          '${1}:${name}:$$\n',
          '--macro-send',
          'READY',
          'WRONG\n',
          '--macro-command',
          '^\\u{1b}\\[31mERROR (?<code>[0-9]+)$',
          'tool-${code}',
          '--macro-arg',
          '${0}',
          '--macro-arg',
          '$${1}',
          '--return',
          'lf',
          '--local',
          '--',
          '/bin/sh',
          '-c',
          'stty raw -echo; printf "READY 記者"; IFS= read -r value; printf "\\r\\nSENT:%s\\r\\n\\033[31mERROR 42\\r\\n" "$value"; exec cat',
        ]);
        await toPass(async () => {
          const output = (await app.output()).stdout;
          const cooked = output
            .split('\n')
            .filter((line) => line.startsWith('cooked='))
            .map((line) => Buffer.from(line.slice(7), 'base64').toString())
            .join('');
          expect(cooked).toContain('SENT:記者:記者:$\r\n');
          expect(cooked).not.toContain('WRONG');
          const commands = output
            .split('\n')
            .filter((line) => line.startsWith('macro='))
            .map((line) =>
              Buffer.from(line.slice(6), 'base64').toString().split('\0')
            );
          expect(commands).toEqual([['tool-42', '\x1b[31mERROR 42', '${1}']]);
          const state = output
            .split('\n')
            .reverse()
            .find((line) => line.startsWith('backend='));
          expect(state).toContain(`backend=${backend}`);
          expect(state).toContain('ready=1');
        });
      } finally {
        await launcher.release();
      }
    });
  }
});
