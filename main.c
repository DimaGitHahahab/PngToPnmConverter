#include "return_codes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ZLIB
#include <zlib.h>
#elif defined(LIBDEFLATE)
#include <libdeflate.h>
#elif defined(ISAL)
#error "ISA-L is currently unsupported."
#else
#error "Error: no uncompression library specified."
#endif

typedef unsigned char byte;
typedef unsigned int uint;

typedef struct {
    uint data_size;
    byte type[4];
    byte *data;
} chunk;

uint bigEndianToUint32(const byte *buf) {
    return (uint) buf[0] << 24 | (uint) buf[1] << 16 | (uint) buf[2] << 8 | (uint) buf[3];
}

int readPngSignature(FILE *file) {
    byte signature[8];
    if (fread(&signature, sizeof(byte), sizeof(signature), file) != 8) {
        fprintf(stderr, "Can't read signature of the file\n");
        return ERROR_DATA_INVALID;
    }

    if (!(signature[0] == 0x89 && signature[1] == 'P' && signature[2] == 'N' && signature[3] == 'G' &&
          signature[4] == 0x0D && signature[5] == 0x0A && signature[6] == 0x1A && signature[7] == 0x0A)) {
        fprintf(stderr, "Invalid PNG signature\n");
        return ERROR_DATA_INVALID;
    }
    return SUCCESS;
}

int readChunk(FILE *file, uint *data_size, byte *type) {
    byte tempSize[4];
    if (fread(&tempSize, sizeof(byte), 4, file) != 4) {
        fprintf(stderr, "Can't read chunk size\n");
        return ERROR_DATA_INVALID;
    }
    *data_size = bigEndianToUint32(tempSize);

    if (fread(type, sizeof(byte), 4, file) != 4) {
        fprintf(stderr, "Can't read chunk type\n");
        return ERROR_DATA_INVALID;
    }

    return SUCCESS;
}

int readIHDRChunk(FILE *file, chunk *ihdr) {
    if (readChunk(file, &(ihdr->data_size), ihdr->type) != SUCCESS) {
        return ERROR_DATA_INVALID;
    }
    if (ihdr->data_size != 13) {
        fprintf(stderr, "IHDR data size is incorrect. Expected: 13, got: %i\n", ihdr->data_size);
        return ERROR_DATA_INVALID;
    }
    if (!(ihdr->type[0] == 'I' && ihdr->type[1] == 'H' && ihdr->type[2] == 'D' && ihdr->type[3] == 'R')) {
        fprintf(stderr, "First chunk's type is incorrect. Expected: IHDR, got %s\n", ihdr->type);
        return ERROR_DATA_INVALID;
    }
    ihdr->data = malloc(ihdr->data_size);
    if (ihdr->data == NULL) {
        free(ihdr->data);
        fprintf(stderr, "Not enough memory for IHDR data size\n");
        return ERROR_OUT_OF_MEMORY;
    }

    if (fread(ihdr->data, sizeof(byte), ihdr->data_size, file) != ihdr->data_size) {
        free(ihdr->data);
        fprintf(stderr, "Can't read IHDR chunk's data\n");
        return ERROR_DATA_INVALID;
    }
    if (ihdr->data[8] != 8) {
        free(ihdr->data);
        fprintf(stderr, "Unsupported bit depth. Expected: 8, got: %c\n", ihdr->data[8]);
        return ERROR_UNSUPPORTED;
    }
    fseek(file, 4, SEEK_CUR);
    return SUCCESS;
}

int readAndProcessChunks(FILE *file, byte **buffer, uint *bufferSize, chunk *plte, uint colorType) {
    uint data_size;
    byte type[4];
    while (!feof(file)) {
        if (readChunk(file, &data_size, type) != SUCCESS) {
            return ERROR_DATA_INVALID;
        }
        if (type[0] == 'I' && type[1] == 'E' && type[2] == 'N' && type[3] == 'D') {
            if (*buffer == NULL) {
                fprintf(stderr, "IEND chunk is before IDAT chunk\n");
                return ERROR_DATA_INVALID;
            }
            break;
        }
        if (type[0] == 'I' && type[1] == 'D' && type[2] == 'A' && type[3] == 'T') {
            byte *tempBuffer = realloc(*buffer, *bufferSize + data_size);
            if (tempBuffer == NULL) {
                fprintf(stderr, "Not enough memory for saving IDAT data\n");
                return ERROR_OUT_OF_MEMORY;
            }
            *buffer = tempBuffer;
            if (fread(*buffer + *bufferSize, sizeof(byte), data_size, file) != data_size) {
                fprintf(stderr, "\n");
                return ERROR_DATA_INVALID;
            }
            *bufferSize += data_size;
            fseek(file, 4, SEEK_CUR);
        } else if (type[0] == 'P' && type[1] == 'L' && type[2] == 'T' && type[3] == 'E') {
            if (colorType != 3) {
                fprintf(stderr, "PLTE chunk is only allowed for color type 3\n");
                return ERROR_DATA_INVALID;
            }
            plte->data_size = data_size;
            memcpy(plte->type, type, 4);
            plte->data = malloc(plte->data_size);
            if (plte->data == NULL) {
                fprintf(stderr, "Not enough memory for saving PLTE data\n");
                return ERROR_OUT_OF_MEMORY;
            }
            if (fread(plte->data, sizeof(byte), data_size, file) != data_size) {
                free(plte->data);
                fprintf(stderr, "Can't read PLTE chunk's data\n");
                return ERROR_DATA_INVALID;
            }
            fseek(file, 4, SEEK_CUR);
        } else {
            if (fseek(file, data_size + 4, SEEK_CUR) != 0) {
                fprintf(stderr, "Can't skip extra chunk\n");
                return ERROR_DATA_INVALID;
            }
        }
    }
    if (*buffer == NULL) {
        fprintf(stderr, "No IDAT chunks\n");
        return ERROR_DATA_INVALID;
    }

    return SUCCESS;
}

#ifdef ZLIB
int zlibUncompress(uint* uncompressedBufferSize, byte* uncompressedBuffer, const byte* buffer, uint bufferSize)
{
    Bytef* dest = (Bytef*)uncompressedBuffer;
    const Bytef* source = (const Bytef*)buffer;
    uLong sourceLen = (uLong)bufferSize;
    int err = uncompress(dest, (uLongf*)&uncompressedBufferSize, source, sourceLen);
    switch (err)
    {
    case Z_OK:
        return SUCCESS;
    case Z_MEM_ERROR:
        fprintf(stderr, "Not enough memory for uncompress with zlib\n");
        return ERROR_OUT_OF_MEMORY;
    case Z_BUF_ERROR:
        fprintf(stderr, "Buffer error while uncompress with zlib\n");
        return ERROR_DATA_INVALID;
    default:
        fprintf(stderr, "Can't uncompress data with zlib\n");
        return ERROR_DATA_INVALID;
    }
}
#endif

#ifdef LIBDEFLATE
int libdeflateUncompress(const uint* uncompressedBufferSize, byte* uncompressedBuffer, const byte* buffer, uint bufferSize)
{
    struct libdeflate_decompressor* decompressor = libdeflate_alloc_decompressor();
    if (decompressor == NULL)
    {
        fprintf(stderr, "Not enough memory for uncompressing with libdeflate\n");
        return ERROR_OUT_OF_MEMORY;
    }

    size_t actual_out_nbytes;
    enum libdeflate_result err =
        libdeflate_zlib_decompress(decompressor, buffer, bufferSize, uncompressedBuffer, *uncompressedBufferSize, &actual_out_nbytes);
    libdeflate_free_decompressor(decompressor);

    switch (err)
    {
    case LIBDEFLATE_SUCCESS:
        return SUCCESS;
    case LIBDEFLATE_BAD_DATA:
        fprintf(stderr, "Can't uncompress data with zlib\n");
        return ERROR_DATA_INVALID;
    case LIBDEFLATE_INSUFFICIENT_SPACE:
        fprintf(stderr, "Buffer error while uncompress with zlib\n");
        return ERROR_DATA_INVALID;
    default:
        fprintf(stderr, "Can't uncompress data with libdeflate\n");
        return ERROR_DATA_INVALID;
    }
}

#endif

int uncompressData(uint *uncompressedBufferSize, byte *uncompressedBuffer, const byte *buffer, uint bufferSize) {
#ifdef ZLIB
    return zlibUncompress(uncompressedBufferSize, uncompressedBuffer, buffer, bufferSize);
#elif LIBDEFLATE
    return libdeflateUncompress(uncompressedBufferSize, uncompressedBuffer, buffer, bufferSize);
#elif ISAL
#error "ISA-L library is unsupported"
#else
#error "Error: no uncompression library specified."
#endif
}

int getPNMFormat(chunk *plte, int *format) {
    if (plte->data_size % 3 != 0) {
        return ERROR_DATA_INVALID;
    }
    uint n = plte->data_size / 3;
    for (int i = 0; i < n; i++) {
        if (plte->data[i * 3] != plte->data[i * 3 + 1] || plte->data[i * 3] != plte->data[i * 3 + 2]) {
            *format = 6;
            return SUCCESS;
        }
    }
    *format = 5;
    return SUCCESS;
}

void applySubFilter(byte *scanline, uint rowSize, int bytesPerPixel) {
    for (uint i = bytesPerPixel; i < rowSize; i++) {
        scanline[i] = (scanline[i] + scanline[i - bytesPerPixel]) % 256;
    }
}

void applyUpFilter(byte *uncompressedBuffer, uint rowIndex, uint rowSize) {
    if (rowIndex == 0) {
        return;
    }

    byte *currentScanline = uncompressedBuffer + rowIndex * (rowSize + 1) + 1;
    byte *prevScanline = uncompressedBuffer + (rowIndex - 1) * (rowSize + 1) + 1;

    for (uint i = 0; i < rowSize; i++) {
        currentScanline[i] = (currentScanline[i] + prevScanline[i]) % 256;
    }
}

void applyAverageFilter(byte *uncompressedBuffer, uint rowIndex, uint rowSize, int bytesPerPixel) {
    byte *currentScanline = uncompressedBuffer + rowIndex * (rowSize + 1) + 1;
    byte *prevScanline = (rowIndex == 0) ? NULL : uncompressedBuffer + (rowIndex - 1) * (rowSize + 1) + 1;

    for (uint i = 0; i < rowSize; i++) {
        byte a = (i < bytesPerPixel) ? 0 : currentScanline[i - bytesPerPixel];
        byte b = (prevScanline == NULL) ? 0 : prevScanline[i];
        currentScanline[i] = (currentScanline[i] + (a + b) / 2) % 256;
    }
}

byte paethPredictor(int a, int b, int c) {
    int p = a + b - c;
    int pa = abs(p - a);
    int pb = abs(p - b);
    int pc = abs(p - c);

    if (pa <= pb && pa <= pc)
        return (byte) a;
    else if (pb <= pc)
        return (byte) b;
    else
        return (byte) c;
}

void applyPaethFilter(byte *uncompressedBuffer, uint rowIndex, uint rowSize, int bytesPerPixel) {
    byte *currentScanline = uncompressedBuffer + rowIndex * (rowSize + 1) + 1;
    byte *prevScanline = (rowIndex == 0) ? NULL : uncompressedBuffer + (rowIndex - 1) * (rowSize + 1) + 1;

    for (uint i = 0; i < rowSize; i++) {
        byte a = (i < bytesPerPixel) ? 0 : currentScanline[i - bytesPerPixel];
        byte b = (prevScanline == NULL) ? 0 : prevScanline[i];
        byte c = (i < bytesPerPixel || prevScanline == NULL) ? 0 : prevScanline[i - bytesPerPixel];

        currentScanline[i] = (currentScanline[i] + paethPredictor(a, b, c)) % 256;
    }
}

int applyFilters(byte *uncompressedBuffer, uint width, uint height, uint colorType) {
    int bytesPerPixel = (colorType == 2) ? 3 : 1;
    uint rowSize = width * bytesPerPixel;

    for (uint rowIndex = 0; rowIndex < height; rowIndex++) {
        byte filterType = uncompressedBuffer[rowIndex * (rowSize + 1)];

        switch (filterType) {
            case 0:
                break;
            case 1:
                applySubFilter(uncompressedBuffer + rowIndex * (rowSize + 1) + 1, rowSize, bytesPerPixel);
                break;
            case 2:
                applyUpFilter(uncompressedBuffer, rowIndex, rowSize);
                break;
            case 3:
                applyAverageFilter(uncompressedBuffer, rowIndex, rowSize, bytesPerPixel);
                break;
            case 4:
                applyPaethFilter(uncompressedBuffer, rowIndex, rowSize, bytesPerPixel);
                break;
            default:
                fprintf(stderr, "Invalid filter type: %u\n", filterType);
                return ERROR_DATA_INVALID;
        }
    }

    return SUCCESS;
}

int
writePNM(FILE *pnmFile, byte *uncompressedBuffer, uint uncompressedBufferSize, uint width, uint height, uint colorType,
         chunk *plte, int format) {
    switch (colorType) {
        case 0:
            fprintf(pnmFile, "P5\n");
            fprintf(pnmFile, "%u %u\n", width, height);
            fprintf(pnmFile, "255\n");
            for (int i = 0; i < width * height * 3; i++) {
                if (i % width != 0) {
                    if (fwrite(uncompressedBuffer + i, sizeof(byte), 1, pnmFile) != 1) {
                        return ERROR_UNKNOWN;
                    }
                }
            }
            break;
        case 2:
            fprintf(pnmFile, "P6\n");
            fprintf(pnmFile, "%u %u\n", width, height);
            fprintf(pnmFile, "255\n");
            for (int i = 0; i < height; ++i) {
                if (fwrite(uncompressedBuffer + i * (width * 3 + 1) + 1, sizeof(byte), width * 3, pnmFile) !=
                    width * 3) {
                    return ERROR_UNKNOWN;
                }
            }
            break;
        default:
            fprintf(pnmFile, "P%i\n", format);
            fprintf(pnmFile, "%u %u\n", width, height);
            fprintf(pnmFile, "255\n");
            for (int i = 0; i < uncompressedBufferSize; i++) {
                if (i % width != 0) {
                    if (fwrite(plte->data + uncompressedBuffer[i] * 3, sizeof(byte), 3, pnmFile) != 3) {
                        return ERROR_UNKNOWN;
                    }
                }
            }
    }
    return SUCCESS;
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Wrong number of arguments\n");
        return ERROR_PARAMETER_INVALID;
    }

    FILE *pngFile = fopen(argv[1], "rb");
    if (pngFile == NULL) {
        fprintf(stderr, "Can't open %s\n", argv[1]);
        return ERROR_CANNOT_OPEN_FILE;
    }

    int err = readPngSignature(pngFile);
    if (err != SUCCESS) {
        fclose(pngFile);
        return err;
    }

    chunk ihdr;
    err = readIHDRChunk(pngFile, &ihdr);
    if (err != SUCCESS) {
        fclose(pngFile);
        return err;
    }

    uint colorType = ihdr.data[9];
    if (!(colorType == 0 || colorType == 2 || colorType == 3)) {
        fprintf(stderr, "Color type %c is unsupported\n", colorType);
        free(ihdr.data);
        fclose(pngFile);
        return ERROR_UNSUPPORTED;
    }

    chunk plte;
    plte.data = NULL;
    byte *buffer = NULL;
    uint bufferSize = 0;
    err = readAndProcessChunks(pngFile, &buffer, &bufferSize, &plte, colorType);
    if (err != SUCCESS) {
        free(ihdr.data);
        free(plte.data);
        free(buffer);
        fclose(pngFile);
        return err;
    }
    if (plte.data != NULL && colorType != 3) {
        fprintf(stderr, "PLTE chunk is only allowed for color type 3\n");
        free(ihdr.data);
        free(plte.data);
        free(buffer);
        fclose(pngFile);
        return ERROR_DATA_INVALID;
    }
    fclose(pngFile);

    int format;
    err = getPNMFormat(&plte, &format);
    if (err != SUCCESS) {
        free(ihdr.data);
        free(plte.data);
        free(buffer);
        return err;
    }
    uint width = bigEndianToUint32(ihdr.data);
    uint height = bigEndianToUint32(ihdr.data + 4);
    uint uncompressedBufferSize = width * height * 5;
    byte *uncompressedBuffer = malloc(uncompressedBufferSize);
    if (uncompressedBuffer == NULL) {
        free(ihdr.data);
        free(buffer);
        fprintf(stderr, "Not enough memory\n");
        return ERROR_OUT_OF_MEMORY;
    }
    err = uncompressData(&uncompressedBufferSize, uncompressedBuffer, buffer, bufferSize);
    if (err != SUCCESS) {
        free(ihdr.data);
        free(buffer);
        free(uncompressedBuffer);
        return err;
    }
    free(buffer);

    err = applyFilters(uncompressedBuffer, width, height, colorType);
    if (err != SUCCESS) {
        free(ihdr.data);
        free(uncompressedBuffer);
        return err;
    }

    FILE *pnmFile = fopen(argv[2], "wb");
    if (pnmFile == NULL) {
        free(ihdr.data);
        free(uncompressedBuffer);
        fprintf(stderr, "Can't open %s\n", argv[2]);
        return ERROR_CANNOT_OPEN_FILE;
    }

    err = writePNM(pnmFile, uncompressedBuffer, uncompressedBufferSize, width, height, colorType, &plte, format);
    if (err != SUCCESS) {
        free(ihdr.data);
        free(uncompressedBuffer);
        return err;
    }

    fclose(pnmFile);
    free(ihdr.data);
    free(uncompressedBuffer);
    return SUCCESS;
}