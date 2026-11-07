/**
 * A dumped game folder: the decrypted NCAs a title is made of (§1.6 - the
 * player's own dump, decrypted with their own tools). The Program NCA is
 * what boots; the rest (the control NCA with the game's name and icon,
 * an update, DLC) go to the core beside it as the "(game folder)" mod,
 * where the loader picks out what it uses by content type and program id
 * (core/emulator.c find_update, find_addons, find_control). Still-encrypted
 * NCAs are counted and left out; Voland never decrypts.
 */

/** NCA header fields (plaintext header, nca_parse.h). */
const NCA_MAGIC_OFFSET = 0x200;
const NCA_CONTENT_TYPE_OFFSET = 0x205;
const NCA_PROGRAM_ID_OFFSET = 0x210;
const NCA_HEADER_PROBE_BYTES = NCA_PROGRAM_ID_OFFSET + 8;
const NCA_CONTENT_PROGRAM = 0;
/** An update's program id is its base game's plus this (emulator.h). */
const UPDATE_ID_BIT = 0x800n;

export type NcaKind = "program" | "update" | "other" | "encrypted";

export interface NcaInfo {
  readonly kind: NcaKind;
  readonly programId: bigint;
}

/** Classifies an NCA from its first NCA_HEADER_PROBE_BYTES bytes. Pure. */
export function classifyNcaHeader(bytes: Uint8Array): NcaInfo {
  if (bytes.byteLength < NCA_HEADER_PROBE_BYTES) return { kind: "encrypted", programId: 0n };
  const magic = String.fromCharCode(...bytes.subarray(NCA_MAGIC_OFFSET, NCA_MAGIC_OFFSET + 4));
  if (magic !== "NCA3" && magic !== "NCA2") return { kind: "encrypted", programId: 0n };
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const programId = view.getBigUint64(NCA_PROGRAM_ID_OFFSET, true);
  if (bytes[NCA_CONTENT_TYPE_OFFSET] !== NCA_CONTENT_PROGRAM) return { kind: "other", programId };
  return { kind: (programId & UPDATE_ID_BIT) !== 0n ? "update" : "program", programId };
}

export interface GameFolder {
  /** The base game's Program NCA (the largest, if there are several). */
  readonly program: File | null;
  /** Every other decrypted NCA in the folder. */
  readonly extras: readonly File[];
  /** NCAs whose header is not plaintext: left out. */
  readonly encrypted: number;
}

/** Sorts a folder's files into the game and what comes with it. */
export async function scanGameFolder(files: readonly File[]): Promise<GameFolder> {
  const ncas = files.filter((f) => /\.nca$/i.test(f.name));
  const infos = await Promise.all(ncas.map(async (file) =>
    ({ file, info: classifyNcaHeader(new Uint8Array(await file.slice(0, NCA_HEADER_PROBE_BYTES).arrayBuffer())) })));
  let program: File | null = null;
  for (const { file, info } of infos) {
    if (info.kind === "program" && (!program || file.size > program.size)) program = file;
  }
  return {
    program,
    extras: infos.filter(({ file, info }) => file !== program && info.kind !== "encrypted").map(({ file }) => file),
    encrypted: infos.filter(({ info }) => info.kind === "encrypted").length,
  };
}

/** The files directly in `folder` and one level of subfolders (dump tools
 * sometimes put the NCAs in a subfolder). */
export async function folderFiles(folder: FileSystemDirectoryHandle): Promise<File[]> {
  const files: File[] = [];
  const walk = async (dir: FileSystemDirectoryHandle, depth: number): Promise<void> => {
    const entries = (dir as unknown as { values: () => AsyncIterable<FileSystemHandle> }).values();
    for await (const entry of entries) {
      if (entry.kind === "file") files.push(await (entry as FileSystemFileHandle).getFile());
      else if (depth === 0) await walk(entry as FileSystemDirectoryHandle, depth + 1);
    }
  };
  await walk(folder, 0);
  return files;
}

/** What to tell the player when a folder has no game in it. Pure. */
export function noGameMessage(folder: GameFolder): string {
  return folder.encrypted > 0
    ? `No decrypted Program NCA in this folder (${folder.encrypted} encrypted NCA file(s) left out). Decrypt the game first; Voland only loads decrypted NCAs.`
    : "No Program NCA in this folder. Choose the folder your decrypted game's .nca files are in.";
}
