use std::error::Error;
use std::fs::{File, metadata};
use std::io::{Read, Write};
use std::sync::mpsc::{self, Sender, Receiver};
use std::thread;
use std::time::Instant;
use num_cpus;

const CHUNK_SIZE: usize = 64 * 1024; // 64KB per chunk

/// Convert MP4 -> Binary (parallelized)
fn mp4_to_binary(input: &str, output: &str) -> Result<(), Box<dyn Error>> {
    let meta = metadata(input)?;
    let file_size = meta.len();
    println!("Processing MP4 file: {} bytes", file_size);

    let mut input_file = File::open(input)?;
    let output_file = File::create(output)?;
    let mut writer = std::io::BufWriter::new(output_file);

    let mut chunks = Vec::new();
    let mut offset = 0;

    while offset < file_size {
        let size = std::cmp::min(CHUNK_SIZE as u64, file_size - offset) as usize;
        let mut buffer = vec![0u8; size];
        input_file.read_exact(&mut buffer)?;
        chunks.push((offset / CHUNK_SIZE as u64, buffer));
        offset += size as u64;
    }

    let (tx, rx): (Sender<(u64, String)>, Receiver<(u64, String)>) = mpsc::channel();

    for (chunk_index, data) in chunks {
        let tx = tx.clone();
        thread::spawn(move || {
            let binary_chunk: String = data.iter()
                .map(|byte| format!("{:08b}", byte))
                .collect();
            tx.send((chunk_index, binary_chunk)).unwrap();
        });
    }

    drop(tx); // close sending side

    let mut results = Vec::new();
    for received in rx {
        results.push(received);
    }

    // Sort by chunk index to preserve order
    results.sort_by_key(|(i, _)| *i);

    for (_, chunk) in results {
        writer.write_all(chunk.as_bytes())?;
    }

    writer.flush()?;
    println!("✅ Done! Binary string saved to {}", output);
    Ok(())
}

/// Convert Binary -> MP4 (parallelized)
fn binary_to_mp4(input: &str, output: &str) -> Result<(), Box<dyn Error>> {
    let meta = metadata(input)?;
    let file_size = meta.len();
    println!("Processing binary file: {} bytes", file_size);

    let mut input_file = File::open(input)?;
    let output_file = File::create(output)?;
    let mut writer = std::io::BufWriter::new(output_file);

    let mut chunks = Vec::new();
    let mut offset = 0;

    while offset < file_size {
        let size = std::cmp::min(CHUNK_SIZE as u64, file_size - offset) as usize;
        let mut buffer = vec![0u8; size];
        input_file.read_exact(&mut buffer)?;
        chunks.push((offset / CHUNK_SIZE as u64, buffer));
        offset += size as u64;
    }

    let (tx, rx): (Sender<(u64, Vec<u8>)>, Receiver<(u64, Vec<u8>)>) = mpsc::channel();

    for (chunk_index, data) in chunks {
        let tx = tx.clone();
        thread::spawn(move || {
            let chunk_str = String::from_utf8_lossy(&data);
            let valid_bits: String = chunk_str.chars().filter(|c| *c == '0' || *c == '1').collect();

            let mut bytes = Vec::new();
            for byte_str in valid_bits.as_bytes().chunks(8) {
                if byte_str.len() == 8 {
                    if let Ok(s) = std::str::from_utf8(byte_str) {
                        if let Ok(b) = u8::from_str_radix(s, 2) {
                            bytes.push(b);
                        }
                    }
                }
            }
            tx.send((chunk_index, bytes)).unwrap();
        });
    }

    drop(tx);

    let mut results = Vec::new();
    for received in rx {
        results.push(received);
    }

    results.sort_by_key(|(i, _)| *i);

    for (_, chunk) in results {
        writer.write_all(&chunk)?;
    }

    writer.flush()?;
    println!("✅ Done! MP4 saved as {}", output);
    Ok(())
}

fn main() -> Result<(), Box<dyn Error>> {
    let num_threads = num_cpus::get();
    println!("=== MP4 Binary Converter (Multi-threaded) ===");
    println!("Using {} threads", num_threads);

    println!("\n1. Converting MP4 to binary...");
    let start = Instant::now();
    mp4_to_binary("input.mkv", "output_binary.txt")?;
    println!("Took: {:.4} seconds", start.elapsed().as_secs_f64());

    println!("\n2. Converting binary to MP4...");
    let start = Instant::now();
    binary_to_mp4("output_binary.txt", "reconstructed.mkv")?;
    println!("Took: {:.4} seconds", start.elapsed().as_secs_f64());

    println!("\n=== Test Complete ===");
    Ok(())
}
