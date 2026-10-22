import { spawn } from "node:child_process";
import { createHash } from "node:crypto";
import {
  access,
  cp,
  lstat,
  mkdir,
  readFile,
  readdir,
  readlink,
  rm,
  writeFile,
} from "node:fs/promises";
import { availableParallelism } from "node:os";
import { basename, join, relative, resolve, sep } from "node:path";

const [rootArgument, buildArgument] = process.argv.slice(2);
if (rootArgument === undefined || buildArgument === undefined) {
  throw new Error(
    "Usage: prepare-jemalloc <source-directory> <build-directory>",
  );
}

const root = resolve(rootArgument);
const upstream = join(root, "deps/jemalloc");
const buildRoot = resolve(buildArgument);
const build = join(buildRoot, "jemalloc");
const source = join(build, "source");
const signaturePath = join(build, "input-signature");
const outputStamp = join(buildRoot, "jemalloc-build.stamp");
const library = join(build, "lib/libjemalloc.so.2");
const linkerName = join(build, "lib/libjemalloc.so");
const version = "5.4.0-0-g7a34f18502e7b222724097cdcd499b437d189acc";
const configureArgs = [
  "--disable-debug",
  "--disable-doc",
  "--disable-prof",
  "--disable-static",
  `--with-version=${version}`,
];
const jobsArgument = process.env.ELDER_TERMS_MAKE_JOBS;
if (
  jobsArgument !== undefined &&
  (!/^[0-9]+$/.test(jobsArgument) ||
    !Number.isSafeInteger(Number(jobsArgument)) ||
    Number(jobsArgument) < 1)
) {
  throw new Error(
    `ELDER_TERMS_MAKE_JOBS must be a positive integer: ${jobsArgument}`,
  );
}
const makeJobs =
  jobsArgument === undefined
    ? Math.min(availableParallelism(), 8)
    : Number(jobsArgument);

const buildRelativeToUpstream = relative(upstream, build);
if (
  buildRelativeToUpstream === "" ||
  (!buildRelativeToUpstream.startsWith(`..${sep}`) &&
    buildRelativeToUpstream !== "..")
) {
  throw new Error("The jemalloc build directory must be outside deps/jemalloc");
}

const run = async (
  command: string,
  args: string[],
  cwd: string,
): Promise<void> => {
  const child = spawn(command, args, { cwd, stdio: "inherit" });
  const code = await new Promise<number | null>((resolveExit, reject) => {
    child.once("error", reject);
    child.once("exit", resolveExit);
  });
  if (code !== 0) throw new Error(`${command} exited with ${code}`);
};

const exists = async (path: string): Promise<boolean> => {
  try {
    await access(path);
    return true;
  } catch {
    return false;
  }
};

const signature = createHash("sha256");
signature.update(JSON.stringify(configureArgs)).update("\n");
for (const variable of [
  "AR",
  "CC",
  "CXX",
  "CFLAGS",
  "CXXFLAGS",
  "CPPFLAGS",
  "LDFLAGS",
  "LD",
  "RANLIB",
  "EXTRA_CFLAGS",
  "EXTRA_CXXFLAGS",
  "EXTRA_LDFLAGS",
]) {
  signature
    .update(variable)
    .update("=")
    .update(process.env[variable] ?? "")
    .update("\n");
}

const hashSource = async (directory: string): Promise<void> => {
  for (const entry of (await readdir(directory, { withFileTypes: true })).sort(
    (a, b) => a.name.localeCompare(b.name),
  )) {
    if (entry.name === ".git") continue;
    const path = join(directory, entry.name);
    signature.update(relative(upstream, path)).update("\0");
    const information = await lstat(path);
    if (information.isDirectory()) {
      signature.update("directory\0");
      await hashSource(path);
    } else if (information.isSymbolicLink()) {
      signature
        .update("symlink\0")
        .update(await readlink(path))
        .update("\0");
    } else {
      signature
        .update("file\0")
        .update(await readFile(path))
        .update("\0");
    }
  }
};

for (const required of ["configure.ac", "Makefile.in", "COPYING"]) {
  if (!(await exists(join(upstream, required)))) {
    throw new Error("Initialize jemalloc with git submodule update --init");
  }
}
await hashSource(upstream);
const currentSignature = signature.digest("hex");
const configured =
  (await exists(signaturePath)) &&
  (await readFile(signaturePath, "utf8")) === currentSignature &&
  (await exists(join(build, "Makefile"))) &&
  (await exists(join(source, "configure")));

if (!configured) {
  await rm(build, { recursive: true, force: true });
  await mkdir(build, { recursive: true });
  // Autoconf writes into its source tree; keep the upstream submodule untouched.
  await cp(upstream, source, {
    recursive: true,
    filter: (path) => basename(path) !== ".git",
  });
  await run("autoconf", [], source);
  await run(join(source, "configure"), configureArgs, build);
  await writeFile(signaturePath, currentSignature);
}

await run(
  "make",
  [`-j${makeJobs}`, "lib/libjemalloc.so.2", "lib/libjemalloc.so"],
  build,
);
if (!(await exists(library)) || !(await exists(linkerName))) {
  throw new Error("jemalloc did not produce both shared library names");
}
const librarySignature = createHash("sha256")
  .update(await readFile(library))
  .digest("hex");
// Ninja can skip dependent copies and links when the output stamp is unchanged.
if (
  !(await exists(outputStamp)) ||
  (await readFile(outputStamp, "utf8")) !== librarySignature
) {
  await writeFile(outputStamp, librarySignature);
}
