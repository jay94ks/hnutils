# hnfs guidance

- Preserve format-specific semantics; IFileSystem is identity only, never universal CRUD.
- Expose only interfaces actually implemented by a concrete driver. Keep probe and support
  availability separate. Do not advertise a driver merely because its enum or header exists.
- Public stateful classes use private PIMPL, C/I/S/E prefixes, camelCase, header guards and
  libcertpp-style Javadoc. Never expose mutable driver state or native handles.
- Separate sequential archive reader/builders, random-access trees/files, journal inspection,
  CoW publication, snapshots, subvolumes, named streams and multi-volume containers.
- Implement on-disk structures and algorithms locally. Compression belongs in hncomp;
  cryptography uses libcertpp; preserve upstream changes only as numbered refs patches.
- Image-file backends must not access raw disks or implicitly overwrite existing files.
- No automatic extraction, journal replay, repair, formatting or CoW commit on open/destruction.
- Enforce 64-bit offset checks, bounded buffers and bounded metadata pages. Unimplemented
  features fail explicitly instead of being ignored or replaced by another format.
- Keep README's implementation matrix accurate. Concrete drivers are CPIO, ISO9660/Joliet,
  physical-partition UDF, MBR/EBR/GPT and standalone read-only VHD/VHDX with copy-out compaction.
- File-backed I/O uses configurable sliding mmap/MapViewOfFile windows, never whole-file views.
- Partition publication is explicit and non-atomic. Compaction preserves the source and virtual
  capacity; never infer filesystem free space or overwrite the source image automatically.
- Tests use doctest and console-only failure handling; validate with independent reference
  implementations as test-only tools, never as runtime codec/filesystem dependencies.
