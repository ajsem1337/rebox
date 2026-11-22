// SPDX-License-Identifier: MIT OR Apache-2.0
//
// BC1 BCPack decoding with bcpack-rs (MIT OR Apache-2.0), for bcpack-helper.py: built as an extra
// binary of the bcpack-rs crate (the flake's bcpack-serve package). It decodes one chunk at a time
// from stdin to stdout, little-endian:
//   request:  u32 blocks, u32 size, size bytes of BCPack BC1
//   response: i32 status (0, or -1 when the chunk doesn't decode), u32 size, size bytes of BC1
// and exits at end of input.

use std::io::{self, Read, Write};

fn read_u32(input: &mut impl Read) -> io::Result<u32> {
    let mut b = [0u8; 4];
    input.read_exact(&mut b)?;
    Ok(u32::from_le_bytes(b))
}

fn main() -> io::Result<()> {
    let window = bcpack::bc1::Window::default_table();
    let quads = bcpack::bc1::QuadTable::default_table();
    let stdin = io::stdin();
    let stdout = io::stdout();
    let mut input = stdin.lock();
    let mut output = io::BufWriter::new(stdout.lock());
    let mut chunk = Vec::new();

    loop {
        let blocks = match read_u32(&mut input) {
            Ok(n) => n as usize,
            Err(e) if e.kind() == io::ErrorKind::UnexpectedEof => return Ok(()),
            Err(e) => return Err(e),
        };
        let size = read_u32(&mut input)? as usize;
        chunk.resize(size, 0);
        input.read_exact(&mut chunk)?;
        match bcpack::bc1::decode(&chunk, blocks, &window, &quads) {
            Ok(out) => {
                output.write_all(&0i32.to_le_bytes())?;
                output.write_all(&(out.blocks.len() as u32).to_le_bytes())?;
                output.write_all(&out.blocks)?;
            }
            Err(e) => {
                eprintln!("bcpack-serve: {blocks} blocks, {size} bytes: {e:?}");
                output.write_all(&(-1i32).to_le_bytes())?;
                output.write_all(&0u32.to_le_bytes())?;
            }
        }
        output.flush()?;
    }
}
