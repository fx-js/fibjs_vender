#include "src/v8.h"

#if V8_TARGET_ARCH_PPC

// Copyright 2014 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// A Disassembler object is used to disassemble a block of code instruction by
// instruction. The default implementation of the NameConverter object can be
// overriden to modify register names or to do symbol lookup on addresses.
//
// The example below will disassemble a block of code and print it to stdout.
//
//   NameConverter converter;
//   Disassembler d(converter);
//   for (byte* pc = begin; pc < end;) {
//     v8::internal::EmbeddedVector<char, 256> buffer;
//     byte* prev_pc = pc;
//     pc += d.InstructionDecode(buffer, pc);
//     printf("%p    %08x      %s\n",
//            prev_pc, *reinterpret_cast<int32_t*>(prev_pc), buffer);
//   }
//
// The Disassembler class also has a convenience method to disassemble a block
// of code into a FILE*, meaning that the above functionality could also be
// achieved by just calling Disassembler::Disassemble(stdout, begin, end);


#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "src/v8.h"

#if V8_TARGET_ARCH_PPC

#include "src/base/platform/platform.h"
#include "src/disasm.h"
#include "src/macro-assembler.h"
#include "src/ppc/constants-ppc.h"


namespace v8 {
namespace internal {


//------------------------------------------------------------------------------

// Decoder decodes and disassembles instructions into an output buffer.
// It uses the converter to convert register names and call destinations into
// more informative description.
class Decoder {
 public:
  Decoder(const disasm::NameConverter& converter, Vector<char> out_buffer)
      : converter_(converter), out_buffer_(out_buffer), out_buffer_pos_(0) {
    out_buffer_[out_buffer_pos_] = '\0';
  }

  ~Decoder() {}

  // Writes one disassembled instruction into 'buffer' (0-terminated).
  // Returns the length of the disassembled machine instruction in bytes.
  int InstructionDecode(byte* instruction);

 private:
  // Bottleneck functions to print into the out_buffer.
  void PrintChar(const char ch);
  void Print(const char* str);

  // Printing of common values.
  void PrintRegister(int reg);
  void PrintDRegister(int reg);
  int FormatFPRegister(Instruction* instr, const char* format);
  void PrintSoftwareInterrupt(SoftwareInterruptCodes svc);

  // Handle formatting of instructions and their options.
  int FormatRegister(Instruction* instr, const char* option);
  int FormatOption(Instruction* instr, const char* option);
  void Format(Instruction* instr, const char* format);
  void Unknown(Instruction* instr);
  void UnknownFormat(Instruction* instr, const char* opcname);

  void DecodeExt1(Instruction* instr);
  void DecodeExt2(Instruction* instr);
  void DecodeExt4(Instruction* instr);
  void DecodeExt5(Instruction* instr);

  const disasm::NameConverter& converter_;
  Vector<char> out_buffer_;
  int out_buffer_pos_;

  DISALLOW_COPY_AND_ASSIGN(Decoder);
};


// Support for assertions in the Decoder formatting functions.
#define STRING_STARTS_WITH(string, compare_string) \
  (strncmp(string, compare_string, strlen(compare_string)) == 0)


// Append the ch to the output buffer.
void Decoder::PrintChar(const char ch) { out_buffer_[out_buffer_pos_++] = ch; }


// Append the str to the output buffer.
void Decoder::Print(const char* str) {
  char cur = *str++;
  while (cur != '\0' && (out_buffer_pos_ < (out_buffer_.length() - 1))) {
    PrintChar(cur);
    cur = *str++;
  }
  out_buffer_[out_buffer_pos_] = 0;
}


// Print the register name according to the active name converter.
void Decoder::PrintRegister(int reg) {
  Print(converter_.NameOfCPURegister(reg));
}


// Print the double FP register name according to the active name converter.
void Decoder::PrintDRegister(int reg) { Print(FPRegisters::Name(reg)); }


// Print SoftwareInterrupt codes. Factoring this out reduces the complexity of
// the FormatOption method.
void Decoder::PrintSoftwareInterrupt(SoftwareInterruptCodes svc) {
  switch (svc) {
    case kCallRtRedirected:
      Print("call rt redirected");
      return;
    case kBreakpoint:
      Print("breakpoint");
      return;
    default:
      if (svc >= kStopCode) {
        out_buffer_pos_ += SNPrintF(out_buffer_ + out_buffer_pos_, "%d - 0x%x",
                                    svc & kStopCodeMask, svc & kStopCodeMask);
      } else {
        out_buffer_pos_ += SNPrintF(out_buffer_ + out_buffer_pos_, "%d", svc);
      }
      return;
  }
}


// Handle all register based formatting in this function to reduce the
// complexity of FormatOption.
int Decoder::FormatRegister(Instruction* instr, const char* format) {
  DCHECK(format[0] == 'r');

  if ((format[1] == 't') || (format[1] == 's')) {  // 'rt & 'rs register
    int reg = instr->RTValue();
    PrintRegister(reg);
    return 2;
  } else if (format[1] == 'a') {  // 'ra: RA register
    int reg = instr->RAValue();
    PrintRegister(reg);
    return 2;
  } else if (format[1] == 'b') {  // 'rb: RB register
    int reg = instr->RBValue();
    PrintRegister(reg);
    return 2;
  }

  UNREACHABLE();
  return -1;
}


// Handle all FP register based formatting in this function to reduce the
// complexity of FormatOption.
int Decoder::FormatFPRegister(Instruction* instr, const char* format) {
  DCHECK(format[0] == 'D');

  int retval = 2;
  int reg = -1;
  if (format[1] == 't') {
    reg = instr->RTValue();
  } else if (format[1] == 'a') {
    reg = instr->RAValue();
  } else if (format[1] == 'b') {
    reg = instr->RBValue();
  } else if (format[1] == 'c') {
    reg = instr->RCValue();
  } else {
    UNREACHABLE();
  }

  PrintDRegister(reg);

  return retval;
}


// FormatOption takes a formatting string and interprets it based on
// the current instructions. The format string points to the first
// character of the option string (the option escape has already been
// consumed by the caller.)  FormatOption returns the number of
// characters that were consumed from the formatting string.
int Decoder::FormatOption(Instruction* instr, const char* format) {
  switch (format[0]) {
    case 'o': {
      if (instr->Bit(10) == 1) {
        Print("o");
      }
      return 1;
    }
    case '.': {
      if (instr->Bit(0) == 1) {
        Print(".");
      } else {
        Print(" ");  // ensure consistent spacing
      }
      return 1;
    }
    case 'r': {
      return FormatRegister(instr, format);
    }
    case 'D': {
      return FormatFPRegister(instr, format);
    }
    case 'i': {  // int16
      int32_t value = (instr->Bits(15, 0) << 16) >> 16;
      out_buffer_pos_ += SNPrintF(out_buffer_ + out_buffer_pos_, "%d", value);
      return 5;
    }
    case 'u': {  // uint16
      int32_t value = instr->Bits(15, 0);
      out_buffer_pos_ += SNPrintF(out_buffer_ + out_buffer_pos_, "%d", value);
      return 6;
    }
    case 'l': {
      // Link (LK) Bit 0
      if (instr->Bit(0) == 1) {
        Print("l");
      }
      return 1;
    }
    case 'a': {
      // Absolute Address Bit 1
      if (instr->Bit(1) == 1) {
        Print("a");
      }
      return 1;
    }
    case 'c': {  // 'cr: condition register of branch instruction
      int code = instr->Bits(20, 18);
      if (code != 7) {
        out_buffer_pos_ +=
            SNPrintF(out_buffer_ + out_buffer_pos_, " cr%d", code);
      }
      return 2;
    }
    case 't': {  // 'target: target of branch instructions
      // target26 or target16
      DCHECK(STRING_STARTS_WITH(format, "target"));
      if ((format[6] == '2') && (format[7] == '6')) {
        int off = ((instr->Bits(25, 2)) << 8) >> 6;
        out_buffer_pos_ += SNPrintF(
            out_buffer_ + out_buffer_pos_, "%+d -> %s", off,
            converter_.NameOfAddress(reinterpret_cast<byte*>(instr) + off));
        return 8;
      } else if ((format[6] == '1') && (format[7] == '6')) {
        int off = ((instr->Bits(15, 2)) << 18) >> 16;
        out_buffer_pos_ += SNPrintF(
            out_buffer_ + out_buffer_pos_, "%+d -> %s", off,
            converter_.NameOfAddress(reinterpret_cast<byte*>(instr) + off));
        return 8;
      }
      case 's': {
        DCHECK(format[1] == 'h');
        int32_t value = 0;
        int32_t opcode = instr->OpcodeValue() << 26;
        int32_t sh = instr->Bits(15, 11);
        if (opcode == EXT5 ||
            (opcode == EXT2 && instr->Bits(10, 2) << 2 == SRADIX)) {
          // SH Bits 1 and 15-11 (split field)
          value = (sh | (instr->Bit(1) << 5));
        } else {
          // SH Bits 15-11
          value = (sh << 26) >> 26;
        }
        out_buffer_pos_ += SNPrintF(out_buffer_ + out_buffer_pos_, "%d", value);
        return 2;
      }
      case 'm': {
        int32_t value = 0;
        if (format[1] == 'e') {
          if (instr->OpcodeValue() << 26 != EXT5) {
            // ME Bits 10-6
            value = (instr->Bits(10, 6) << 26) >> 26;
          } else {
            // ME Bits 5 and 10-6 (split field)
            value = (instr->Bits(10, 6) | (instr->Bit(5) << 5));
          }
        } else if (format[1] == 'b') {
          if (instr->OpcodeValue() << 26 != EXT5) {
            // MB Bits 5-1
            value = (instr->Bits(5, 1) << 26) >> 26;
          } else {
            // MB Bits 5 and 10-6 (split field)
            value = (instr->Bits(10, 6) | (instr->Bit(5) << 5));
          }
        } else {
          UNREACHABLE();  // bad format
        }
        out_buffer_pos_ += SNPrintF(out_buffer_ + out_buffer_pos_, "%d", value);
        return 2;
      }
    }
#if V8_TARGET_ARCH_PPC64
    case 'd': {  // ds value for offset
      int32_t value = SIGN_EXT_IMM16(instr->Bits(15, 0) & ~3);
      out_buffer_pos_ += SNPrintF(out_buffer_ + out_buffer_pos_, "%d", value);
      return 1;
    }
#endif
    default: {
      UNREACHABLE();
      break;
    }
  }

  UNREACHABLE();
  return -1;
}


// Format takes a formatting string for a whole instruction and prints it into
// the output buffer. All escaped options are handed to FormatOption to be
// parsed further.
void Decoder::Format(Instruction* instr, const char* format) {
  char cur = *format++;
  while ((cur != 0) && (out_buffer_pos_ < (out_buffer_.length() - 1))) {
    if (cur == '\'') {  // Single quote is used as the formatting escape.
      format += FormatOption(instr, format);
    } else {
      out_buffer_[out_buffer_pos_++] = cur;
    }
    cur = *format++;
  }
  out_buffer_[out_buffer_pos_] = '\0';
}


// The disassembler may end up decoding data inlined in the code. We do not want
// it to crash if the data does not ressemble any known instruction.
#define VERIFY(condition) \
  if (!(condition)) {     \
    Unknown(instr);       \
    return;               \
  }


// For currently unimplemented decodings the disassembler calls Unknown(instr)
// which will just print "unknown" of the instruction bits.
void Decoder::Unknown(Instruction* instr) { Format(instr, "unknown"); }


// For currently unimplemented decodings the disassembler calls
// UnknownFormat(instr) which will just print opcode name of the
// instruction bits.
void Decoder::UnknownFormat(Instruction* instr, const char* name) {
  char buffer[100];
  snprintf(buffer, sizeof(buffer), "%s (unknown-format)", name);
  Format(instr, buffer);
}


void Decoder::DecodeExt1(Instruction* instr) {
  switch (instr->Bits(10, 1) << 1) {
    case MCRF: {
      UnknownFormat(instr, "mcrf");  // not used by V8
      break;
    }
    case BCLRX: {
      int bo = instr->Bits(25, 21) << 21;
      int bi = instr->Bits(20, 16);
      CRBit cond = static_cast<CRBit>(bi & (CRWIDTH - 1));
      switch (bo) {
        case DCBNZF: {
          UnknownFormat(instr, "bclrx-dcbnzf");
          break;
        }
        case DCBEZF: {
          UnknownFormat(instr, "bclrx-dcbezf");
          break;
        }
        case BF: {
          switch (cond) {
            case CR_EQ:
              Format(instr, "bnelr'l'cr");
              break;
            case CR_GT:
              Format(instr, "blelr'l'cr");
              break;
            case CR_LT:
              Format(instr, "bgelr'l'cr");
              break;
            case CR_SO:
              Format(instr, "bnsolr'l'cr");
              break;
          }
          break;
        }
        case DCBNZT: {
          UnknownFormat(instr, "bclrx-dcbbzt");
          break;
        }
        case DCBEZT: {
          UnknownFormat(instr, "bclrx-dcbnezt");
          break;
        }
        case BT: {
          switch (cond) {
            case CR_EQ:
              Format(instr, "beqlr'l'cr");
              break;
            case CR_GT:
              Format(instr, "bgtlr'l'cr");
              break;
            case CR_LT:
              Format(instr, "bltlr'l'cr");
              break;
            case CR_SO:
              Format(instr, "bsolr'l'cr");
              break;
          }
          break;
        }
        case DCBNZ: {
          UnknownFormat(instr, "bclrx-dcbnz");
          break;
        }
        case DCBEZ: {
          UnknownFormat(instr, "bclrx-dcbez");  // not used by V8
          break;
        }
        case BA: {
          Format(instr, "blr'l");
          break;
        }
      }
      break;
    }
    case BCCTRX: {
      switch (instr->Bits(25, 21) << 21) {
        case DCBNZF: {
          UnknownFormat(instr, "bcctrx-dcbnzf");
          break;
        }
        case DCBEZF: {
          UnknownFormat(instr, "bcctrx-dcbezf");
          break;
        }
        case BF: {
          UnknownFormat(instr, "bcctrx-bf");
          break;
        }
        case DCBNZT: {
          UnknownFormat(instr, "bcctrx-dcbnzt");
          break;
        }
        case DCBEZT: {
          UnknownFormat(instr, "bcctrx-dcbezf");
          break;
        }
        case BT: {
          UnknownFormat(instr, "bcctrx-bt");
          break;
        }
        case DCBNZ: {
          UnknownFormat(instr, "bcctrx-dcbnz");
          break;
        }
        case DCBEZ: {
          UnknownFormat(instr, "bcctrx-dcbez");
          break;
        }
        case BA: {
          if (instr->Bit(0) == 1) {
            Format(instr, "bctrl");
          } else {
            Format(instr, "bctr");
          }
          break;
        }
        default: { UNREACHABLE(); }
      }
      break;
    }
    case CRNOR: {
      Format(instr, "crnor (stuff)");
      break;
    }
    case RFI: {
      Format(instr, "rfi (stuff)");
      break;
    }
    case CRANDC: {
      Format(instr, "crandc (stuff)");
      break;
    }
    case ISYNC: {
      Format(instr, "isync (stuff)");
      break;
    }
    case CRXOR: {
      Format(instr, "crxor (stuff)");
      break;
    }
    case CRNAND: {
      UnknownFormat(instr, "crnand");
      break;
    }
    case CRAND: {
      UnknownFormat(instr, "crand");
      break;
    }
    case CREQV: {
      UnknownFormat(instr, "creqv");
      break;
    }
    case CRORC: {
      UnknownFormat(instr, "crorc");
      break;
    }
    case CROR: {
      UnknownFormat(instr, "cror");
      break;
    }
    default: {
      Unknown(instr);  // not used by V8
    }
  }
}


void Decoder::DecodeExt2(Instruction* instr) {
  // Some encodings are 10-1 bits, handle those first
  switch (instr->Bits(10, 1) << 1) {
    case SRWX: {
      Format(instr, "srw'.    'ra, 'rs, 'rb");
      return;
    }
#if V8_TARGET_ARCH_PPC64
    case SRDX: {
      Format(instr, "srd'.    'ra, 'rs, 'rb");
      return;
    }
#endif
    case SRAW: {
      Format(instr, "sraw'.   'ra, 'rs, 'rb");
      return;
    }
#if V8_TARGET_ARCH_PPC64
    case SRAD: {
      Format(instr, "srad'.   'ra, 'rs, 'rb");
      return;
    }
#endif
    case SRAWIX: {
      Format(instr, "srawi'.  'ra,'rs,'sh");
      return;
    }
    case EXTSH: {
      Format(instr, "extsh'.  'ra, 'rs");
      return;
    }
#if V8_TARGET_ARCH_PPC64
    case EXTSW: {
      Format(instr, "extsw'.  'ra, 'rs");
      return;
    }
#endif
    case EXTSB: {
      Format(instr, "extsb'.  'ra, 'rs");
      return;
    }
    case LFSX: {
      Format(instr, "lfsx    'rt, 'ra, 'rb");
      return;
    }
    case LFSUX: {
      Format(instr, "lfsux   'rt, 'ra, 'rb");
      return;
    }
    case LFDX: {
      Format(instr, "lfdx    'rt, 'ra, 'rb");
      return;
    }
    case LFDUX: {
      Format(instr, "lfdux   'rt, 'ra, 'rb");
      return;
    }
    case STFSX: {
      Format(instr, "stfsx    'rs, 'ra, 'rb");
      return;
    }
    case STFSUX: {
      Format(instr, "stfsux   'rs, 'ra, 'rb");
      return;
    }
    case STFDX: {
      Format(instr, "stfdx    'rs, 'ra, 'rb");
      return;
    }
    case STFDUX: {
      Format(instr, "stfdux   'rs, 'ra, 'rb");
      return;
    }
  }

  switch (instr->Bits(10, 2) << 2) {
    case SRADIX: {
      Format(instr, "sradi'.  'ra,'rs,'sh");
      return;
    }
  }

  // ?? are all of these xo_form?
  switch (instr->Bits(9, 1) << 1) {
    case CMP: {
#if V8_TARGET_ARCH_PPC64
      if (instr->Bit(21)) {
#endif
        Format(instr, "cmp     'ra, 'rb");
#if V8_TARGET_ARCH_PPC64
      } else {
        Format(instr, "cmpw    'ra, 'rb");
      }
#endif
      return;
    }
    case SLWX: {
      Format(instr, "slw'.   'ra, 'rs, 'rb");
      return;
    }
#if V8_TARGET_ARCH_PPC64
    case SLDX: {
      Format(instr, "sld'.   'ra, 'rs, 'rb");
      return;
    }
#endif
    case SUBFCX: {
      Format(instr, "subfc'. 'rt, 'ra, 'rb");
      return;
    }
    case ADDCX: {
      Format(instr, "addc'.   'rt, 'ra, 'rb");
      return;
    }
    case CNTLZWX: {
      Format(instr, "cntlzw'. 'ra, 'rs");
      return;
    }
#if V8_TARGET_ARCH_PPC64
    case CNTLZDX: {
      Format(instr, "cntlzd'. 'ra, 'rs");
      return;
    }
#endif
    case ANDX: {
      Format(instr, "and'.    'ra, 'rs, 'rb");
      return;
    }
    case ANDCX: {
      Format(instr, "andc'.   'ra, 'rs, 'rb");
      return;
    }
    case CMPL: {
#if V8_TARGET_ARCH_PPC64
      if (instr->Bit(21)) {
#endif
        Format(instr, "cmpl    'ra, 'rb");
#if V8_TARGET_ARCH_PPC64
      } else {
        Format(instr, "cmplw   'ra, 'rb");
      }
#endif
      return;
    }
    case NEGX: {
      Format(instr, "neg'.    'rt, 'ra");
      return;
    }
    case NORX: {
      Format(instr, "nor'.    'rt, 'ra, 'rb");
      return;
    }
    case SUBFX: {
      Format(instr, "subf'.   'rt, 'ra, 'rb");
      return;
    }
    case MULHWX: {
      Format(instr, "mulhw'o'.  'rt, 'ra, 'rb");
      return;
    }
    case ADDZEX: {
      Format(instr, "addze'.   'rt, 'ra");
      return;
    }
    case MULLW: {
      Format(instr, "mullw'o'.  'rt, 'ra, 'rb");
      return;
    }
#if V8_TARGET_ARCH_PPC64
    case MULLD: {
      Format(instr, "mulld'o'.  'rt, 'ra, 'rb");
      return;
    }
#endif
    case DIVW: {
      Format(instr, "divw'o'.   'rt, 'ra, 'rb");
      return;
    }
    case DIVWU: {
      Format(instr, "divwu'o'.  'rt, 'ra, 'rb");
      return;
    }
#if V8_TARGET_ARCH_PPC64
    case DIVD: {
      Format(instr, "divd'o'.   'rt, 'ra, 'rb");
      return;
    }
#endif
    case ADDX: {
      Format(instr, "add'o     'rt, 'ra, 'rb");
      return;
    }
    case XORX: {
      Format(instr, "xor'.    'ra, 'rs, 'rb");
      return;
    }
    case ORX: {
      if (instr->RTValue() == instr->RBValue()) {
        Format(instr, "mr      'ra, 'rb");
      } else {
        Format(instr, "or      'ra, 'rs, 'rb");
      }
      return;
    }
    case MFSPR: {
      int spr = instr->Bits(20, 11);
      if (256 == spr) {
        Format(instr, "mflr    'rt");
      } else {
        Format(instr, "mfspr   'rt ??");
      }
      return;
    }
    case MTSPR: {
      int spr = instr->Bits(20, 11);
      if (256 == spr) {
        Format(instr, "mtlr    'rt");
      } else if (288 == spr) {
        Format(instr, "mtctr   'rt");
      } else {
        Format(instr, "mtspr   'rt ??");
      }
      return;
    }
    case MFCR: {
      Format(instr, "mfcr    'rt");
      return;
    }
    case STWX: {
      Format(instr, "stwx    'rs, 'ra, 'rb");
      return;
    }
    case STWUX: {
      Format(instr, "stwux   'rs, 'ra, 'rb");
      return;
    }
    case STBX: {
      Format(instr, "stbx    'rs, 'ra, 'rb");
      return;
    }
    case STBUX: {
      Format(instr, "stbux   'rs, 'ra, 'rb");
      return;
    }
    case STHX: {
      Format(instr, "sthx    'rs, 'ra, 'rb");
      return;
    }
    case STHUX: {
      Format(instr, "sthux   'rs, 'ra, 'rb");
      return;
    }
    case LWZX: {
      Format(instr, "lwzx    'rt, 'ra, 'rb");
      return;
    }
    case LWZUX: {
      Format(instr, "lwzux   'rt, 'ra, 'rb");
      return;
    }
    case LWAX: {
      Format(instr, "lwax    'rt, 'ra, 'rb");
      return;
    }
    case LBZX: {
      Format(instr, "lbzx    'rt, 'ra, 'rb");
      return;
    }
    case LBZUX: {
      Format(instr, "lbzux   'rt, 'ra, 'rb");
      return;
    }
    case LHZX: {
      Format(instr, "lhzx    'rt, 'ra, 'rb");
      return;
    }
    case LHZUX: {
      Format(instr, "lhzux   'rt, 'ra, 'rb");
      return;
    }
    case LHAX: {
      Format(instr, "lhax    'rt, 'ra, 'rb");
      return;
    }
#if V8_TARGET_ARCH_PPC64
    case LDX: {
      Format(instr, "ldx     'rt, 'ra, 'rb");
      return;
    }
    case LDUX: {
      Format(instr, "ldux    'rt, 'ra, 'rb");
      return;
    }
    case STDX: {
      Format(instr, "stdx    'rt, 'ra, 'rb");
      return;
    }
    case STDUX: {
      Format(instr, "stdux   'rt, 'ra, 'rb");
      return;
    }
    case MFVSRD: {
      Format(instr, "mffprd  'ra, 'Dt");
      return;
    }
    case MFVSRWZ: {
      Format(instr, "mffprwz 'ra, 'Dt");
      return;
    }
    case MTVSRD: {
      Format(instr, "mtfprd  'Dt, 'ra");
      return;
    }
    case MTVSRWA: {
      Format(instr, "mtfprwa 'Dt, 'ra");
      return;
    }
    case MTVSRWZ: {
      Format(instr, "mtfprwz 'Dt, 'ra");
      return;
    }
#endif
  }

  switch (instr->Bits(5, 1) << 1) {
    case ISEL: {
      Format(instr, "isel    'rt, 'ra, 'rb");
      return;
    }
    default: {
      Unknown(instr);  // not used by V8
    }
  }
}


void Decoder::DecodeExt4(Instruction* instr) {
  switch (instr->Bits(5, 1) << 1) {
    case FDIV: {
      Format(instr, "fdiv'.   'Dt, 'Da, 'Db");
      return;
    }
    case FSUB: {
      Format(instr, "fsub'.   'Dt, 'Da, 'Db");
      return;
    }
    case FADD: {
      Format(instr, "fadd'.   'Dt, 'Da, 'Db");
      return;
    }
    case FSQRT: {
      Format(instr, "fsqrt'.  'Dt, 'Db");
      return;
    }
    case FSEL: {
      Format(instr, "fsel'.   'Dt, 'Da, 'Dc, 'Db");
      return;
    }
    case FMUL: {
      Format(instr, "fmul'.   'Dt, 'Da, 'Dc");
      return;
    }
    case FMSUB: {
      Format(instr, "fmsub'.  'Dt, 'Da, 'Dc, 'Db");
      return;
    }
    case FMADD: {
      Format(instr, "fmadd'.  'Dt, 'Da, 'Dc, 'Db");
      return;
    }
  }

  switch (instr->Bits(10, 1) << 1) {
    case FCMPU: {
      Format(instr, "fcmpu   'Da, 'Db");
      break;
    }
    case FRSP: {
      Format(instr, "frsp'.   'Dt, 'Db");
      break;
    }
    case FCFID: {
      Format(instr, "fcfid'.  'Dt, 'Db");
      break;
    }
    case FCTID: {
      Format(instr, "fctid   'Dt, 'Db");
      break;
    }
    case FCTIDZ: {
      Format(instr, "fctidz  'Dt, 'Db");
      break;
    }
    case FCTIW: {
      Format(instr, "fctiw'. 'Dt, 'Db");
      break;
    }
    case FCTIWZ: {
      Format(instr, "fctiwz'. 'Dt, 'Db");
      break;
    }
    case FMR: {
      Format(instr, "fmr'.    'Dt, 'Db");
      break;
    }
    case MTFSFI: {
      Format(instr, "mtfsfi'.  ?,?");
      break;
    }
    case MFFS: {
      Format(instr, "mffs'.   'Dt");
      break;
    }
    case MTFSF: {
      Format(instr, "mtfsf'.  'Db ?,?,?");
      break;
    }
    case FABS: {
      Format(instr, "fabs'.   'Dt, 'Db");
      break;
    }
    case FRIN: {
      Format(instr, "frin.   'Dt, 'Db");
      break;
    }
    case FRIZ: {
      Format(instr, "friz.   'Dt, 'Db");
      break;
    }
    case FRIP: {
      Format(instr, "frip.   'Dt, 'Db");
      break;
    }
    case FRIM: {
      Format(instr, "frim.   'Dt, 'Db");
      break;
    }
    case FNEG: {
      Format(instr, "fneg'.   'Dt, 'Db");
      break;
    }
    default: {
      Unknown(instr);  // not used by V8
    }
  }
}


void Decoder::DecodeExt5(Instruction* instr) {
  switch (instr->Bits(4, 2) << 2) {
    case RLDICL: {
      Format(instr, "rldicl'. 'ra, 'rs, 'sh, 'mb");
      return;
    }
    case RLDICR: {
      Format(instr, "rldicr'. 'ra, 'rs, 'sh, 'me");
      return;
    }
    case RLDIC: {
      Format(instr, "rldic'.  'ra, 'rs, 'sh, 'mb");
      return;
    }
    case RLDIMI: {
      Format(instr, "rldimi'. 'ra, 'rs, 'sh, 'mb");
      return;
    }
  }
  switch (instr->Bits(4, 1) << 1) {
    case RLDCL: {
      Format(instr, "rldcl'.  'ra, 'rs, 'sb, 'mb");
      return;
    }
  }
  Unknown(instr);  // not used by V8
}

#undef VERIFIY

// Disassemble the instruction at *instr_ptr into the output buffer.
int Decoder::InstructionDecode(byte* instr_ptr) {
  Instruction* instr = Instruction::At(instr_ptr);
  // Print raw instruction bytes.
  out_buffer_pos_ += SNPrintF(out_buffer_ + out_buffer_pos_, "%08x       ",
                              instr->InstructionBits());

#if ABI_USES_FUNCTION_DESCRIPTORS
  // The first field will be identified as a jump table entry.  We emit the rest
  // of the structure as zero, so just skip past them.
  if (instr->InstructionBits() == 0) {
    Format(instr, "constant");
    return Instruction::kInstrSize;
  }
#endif

  switch (instr->OpcodeValue() << 26) {
    case TWI: {
      PrintSoftwareInterrupt(instr->SvcValue());
      break;
    }
    case MULLI: {
      UnknownFormat(instr, "mulli");
      break;
    }
    case SUBFIC: {
      Format(instr, "subfic  'rt, 'ra, 'int16");
      break;
    }
    case CMPLI: {
#if V8_TARGET_ARCH_PPC64
      if (instr->Bit(21)) {
#endif
        Format(instr, "cmpli   'ra, 'uint16");
#if V8_TARGET_ARCH_PPC64
      } else {
        Format(instr, "cmplwi  'ra, 'uint16");
      }
#endif
      break;
    }
    case CMPI: {
#if V8_TARGET_ARCH_PPC64
      if (instr->Bit(21)) {
#endif
        Format(instr, "cmpi    'ra, 'int16");
#if V8_TARGET_ARCH_PPC64
      } else {
        Format(instr, "cmpwi   'ra, 'int16");
      }
#endif
      break;
    }
    case ADDIC: {
      Format(instr, "addic   'rt, 'ra, 'int16");
      break;
    }
    case ADDICx: {
      UnknownFormat(instr, "addicx");
      break;
    }
    case ADDI: {
      if (instr->RAValue() == 0) {
        // this is load immediate
        Format(instr, "li      'rt, 'int16");
      } else {
        Format(instr, "addi    'rt, 'ra, 'int16");
      }
      break;
    }
    case ADDIS: {
      if (instr->RAValue() == 0) {
        Format(instr, "lis     'rt, 'int16");
      } else {
        Format(instr, "addis   'rt, 'ra, 'int16");
      }
      break;
    }
    case BCX: {
      int bo = instr->Bits(25, 21) << 21;
      int bi = instr->Bits(20, 16);
      CRBit cond = static_cast<CRBit>(bi & (CRWIDTH - 1));
      switch (bo) {
        case BT: {  // Branch if condition true
          switch (cond) {
            case CR_EQ:
              Format(instr, "beq'l'a'cr 'target16");
              break;
            case CR_GT:
              Format(instr, "bgt'l'a'cr 'target16");
              break;
            case CR_LT:
              Format(instr, "blt'l'a'cr 'target16");
              break;
            case CR_SO:
              Format(instr, "bso'l'a'cr 'target16");
              break;
          }
          break;
        }
        case BF: {  // Branch if condition false
          switch (cond) {
            case CR_EQ:
              Format(instr, "bne'l'a'cr 'target16");
              break;
            case CR_GT:
              Format(instr, "ble'l'a'cr 'target16");
              break;
            case CR_LT:
              Format(instr, "bge'l'a'cr 'target16");
              break;
            case CR_SO:
              Format(instr, "bnso'l'a'cr 'target16");
              break;
          }
          break;
        }
        case DCBNZ: {  // Decrement CTR; branch if CTR != 0
          Format(instr, "bdnz'l'a 'target16");
          break;
        }
        default:
          Format(instr, "bc'l'a'cr 'target16");
          break;
      }
      break;
    }
    case SC: {
      UnknownFormat(instr, "sc");
      break;
    }
    case BX: {
      Format(instr, "b'l'a 'target26");
      break;
    }
    case EXT1: {
      DecodeExt1(instr);
      break;
    }
    case RLWIMIX: {
      Format(instr, "rlwimi'. 'ra, 'rs, 'sh, 'me, 'mb");
      break;
    }
    case RLWINMX: {
      Format(instr, "rlwinm'. 'ra, 'rs, 'sh, 'me, 'mb");
      break;
    }
    case RLWNMX: {
      Format(instr, "rlwnm'.  'ra, 'rs, 'rb, 'me, 'mb");
      break;
    }
    case ORI: {
      Format(instr, "ori     'ra, 'rs, 'uint16");
      break;
    }
    case ORIS: {
      Format(instr, "oris    'ra, 'rs, 'uint16");
      break;
    }
    case XORI: {
      Format(instr, "xori    'ra, 'rs, 'uint16");
      break;
    }
    case XORIS: {
      Format(instr, "xoris   'ra, 'rs, 'uint16");
      break;
    }
    case ANDIx: {
      Format(instr, "andi.   'ra, 'rs, 'uint16");
      break;
    }
    case ANDISx: {
      Format(instr, "andis.  'ra, 'rs, 'uint16");
      break;
    }
    case EXT2: {
      DecodeExt2(instr);
      break;
    }
    case LWZ: {
      Format(instr, "lwz     'rt, 'int16('ra)");
      break;
    }
    case LWZU: {
      Format(instr, "lwzu    'rt, 'int16('ra)");
      break;
    }
    case LBZ: {
      Format(instr, "lbz     'rt, 'int16('ra)");
      break;
    }
    case LBZU: {
      Format(instr, "lbzu    'rt, 'int16('ra)");
      break;
    }
    case STW: {
      Format(instr, "stw     'rs, 'int16('ra)");
      break;
    }
    case STWU: {
      Format(instr, "stwu    'rs, 'int16('ra)");
      break;
    }
    case STB: {
      Format(instr, "stb     'rs, 'int16('ra)");
      break;
    }
    case STBU: {
      Format(instr, "stbu    'rs, 'int16('ra)");
      break;
    }
    case LHZ: {
      Format(instr, "lhz     'rt, 'int16('ra)");
      break;
    }
    case LHZU: {
      Format(instr, "lhzu    'rt, 'int16('ra)");
      break;
    }
    case LHA: {
      Format(instr, "lha     'rt, 'int16('ra)");
      break;
    }
    case LHAU: {
      Format(instr, "lhau    'rt, 'int16('ra)");
      break;
    }
    case STH: {
      Format(instr, "sth 'rs, 'int16('ra)");
      break;
    }
    case STHU: {
      Format(instr, "sthu 'rs, 'int16('ra)");
      break;
    }
    case LMW: {
      UnknownFormat(instr, "lmw");
      break;
    }
    case STMW: {
      UnknownFormat(instr, "stmw");
      break;
    }
    case LFS: {
      Format(instr, "lfs     'Dt, 'int16('ra)");
      break;
    }
    case LFSU: {
      Format(instr, "lfsu    'Dt, 'int16('ra)");
      break;
    }
    case LFD: {
      Format(instr, "lfd     'Dt, 'int16('ra)");
      break;
    }
    case LFDU: {
      Format(instr, "lfdu    'Dt, 'int16('ra)");
      break;
    }
    case STFS: {
      Format(instr, "stfs    'Dt, 'int16('ra)");
      break;
    }
    case STFSU: {
      Format(instr, "stfsu   'Dt, 'int16('ra)");
      break;
    }
    case STFD: {
      Format(instr, "stfd    'Dt, 'int16('ra)");
      break;
    }
    case STFDU: {
      Format(instr, "stfdu   'Dt, 'int16('ra)");
      break;
    }
    case EXT3:
    case EXT4: {
      DecodeExt4(instr);
      break;
    }
    case EXT5: {
      DecodeExt5(instr);
      break;
    }
#if V8_TARGET_ARCH_PPC64
    case LD: {
      switch (instr->Bits(1, 0)) {
        case 0:
          Format(instr, "ld      'rt, 'd('ra)");
          break;
        case 1:
          Format(instr, "ldu     'rt, 'd('ra)");
          break;
        case 2:
          Format(instr, "lwa     'rt, 'd('ra)");
          break;
      }
      break;
    }
    case STD: {  // could be STD or STDU
      if (instr->Bit(0) == 0) {
        Format(instr, "std     'rs, 'd('ra)");
      } else {
        Format(instr, "stdu    'rs, 'd('ra)");
      }
      break;
    }
#endif
    default: {
      Unknown(instr);
      break;
    }
  }

  return Instruction::kInstrSize;
}
}  // namespace internal
}  // namespace v8


//------------------------------------------------------------------------------

namespace disasm {


const char* NameConverter::NameOfAddress(byte* addr) const {
  v8::internal::SNPrintF(tmp_buffer_, "%p", addr);
  return tmp_buffer_.start();
}


const char* NameConverter::NameOfConstant(byte* addr) const {
  return NameOfAddress(addr);
}


const char* NameConverter::NameOfCPURegister(int reg) const {
  return v8::internal::Registers::Name(reg);
}

const char* NameConverter::NameOfByteCPURegister(int reg) const {
  UNREACHABLE();  // PPC does not have the concept of a byte register
  return "nobytereg";
}


const char* NameConverter::NameOfXMMRegister(int reg) const {
  UNREACHABLE();  // PPC does not have any XMM registers
  return "noxmmreg";
}

const char* NameConverter::NameInCode(byte* addr) const {
  // The default name converter is called for unknown code. So we will not try
  // to access any memory.
  return "";
}


//------------------------------------------------------------------------------

Disassembler::Disassembler(const NameConverter& converter)
    : converter_(converter) {}


Disassembler::~Disassembler() {}


int Disassembler::InstructionDecode(v8::internal::Vector<char> buffer,
                                    byte* instruction) {
  v8::internal::Decoder d(converter_, buffer);
  return d.InstructionDecode(instruction);
}


// The PPC assembler does not currently use constant pools.
int Disassembler::ConstantPoolSizeAt(byte* instruction) { return -1; }


void Disassembler::Disassemble(FILE* f, byte* begin, byte* end) {
  NameConverter converter;
  Disassembler d(converter);
  for (byte* pc = begin; pc < end;) {
    v8::internal::EmbeddedVector<char, 128> buffer;
    buffer[0] = '\0';
    byte* prev_pc = pc;
    pc += d.InstructionDecode(buffer, pc);
    v8::internal::PrintF(f, "%p    %08x      %s\n", prev_pc,
                         *reinterpret_cast<int32_t*>(prev_pc), buffer.start());
  }
}


}  // namespace disasm

#endif  // V8_TARGET_ARCH_PPC


#endif  // V8_TARGET_ARCH_PPC