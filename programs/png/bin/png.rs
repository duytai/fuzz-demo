extern crate image;

use std::env;
use std::fs::File;
use std::panic;
use std::process;
use std::io::prelude::*;
use image::{DynamicImage, ImageDecoder};
use image::error::{ImageError, ImageResult, LimitError, LimitErrorKind};

#[inline(always)]
fn png_decode(data: &[u8]) -> ImageResult<DynamicImage> {
    let decoder = image::png::PngDecoder::new(data)?;
    let (width, height) = decoder.dimensions();

    if width.saturating_mul(height) > 4_000_000 {
        return Err(ImageError::Limits(LimitError::from_kind(LimitErrorKind::DimensionError)));
    }

    DynamicImage::from_decoder(decoder)
}

fn main() {
    let args: Vec<String> = env::args().collect();
    let mut buffer = Vec::new();
    let mut f = File::open(&args[1]).unwrap();
    f.read_to_end(&mut buffer).unwrap();
    let was_panic = panic::catch_unwind(|| {
        let _ = png_decode(&buffer);
        println!("OK");
    });
    if was_panic.is_err() {
        process::abort();
    }
}
