#ifndef HALIDE_LLVM_OUTPUTS_H
#define HALIDE_LLVM_OUTPUTS_H

/** \file
 *
 */

#include "Module.h"

namespace llvm {
class Module;
class TargetOptions;
class LLVMContext;
class raw_fd_ostream;
class raw_pwrite_stream;
class raw_ostream;
}

namespace Halide {

namespace Internal {
#if LLVM_VERSION < 37
typedef llvm::raw_ostream LLVMOStream;
#else
typedef llvm::raw_pwrite_stream LLVMOStream;
#endif
}

/** Generate an LLVM module. */
EXPORT std::unique_ptr<llvm::Module> compile_module_to_llvm_module(const Module &module, llvm::LLVMContext &context);

/** Construct an llvm output stream for writing to files. */
std::unique_ptr<llvm::raw_fd_ostream> make_raw_fd_ostream(const std::string &filename);

/** Compile an LLVM module to native targets (objects, native assembly). */
// @{
EXPORT void compile_llvm_module_to_object(llvm::Module &module, Internal::LLVMOStream& out);
EXPORT void compile_llvm_module_to_assembly(llvm::Module &module, Internal::LLVMOStream& out);
// @}

/** Compile an LLVM module to LLVM targets (bitcode, LLVM assembly). */
// @{
EXPORT void compile_llvm_module_to_llvm_bitcode(llvm::Module &module, Internal::LLVMOStream& out);
EXPORT void compile_llvm_module_to_llvm_assembly(llvm::Module &module, Internal::LLVMOStream& out);
// @}

}

#endif
