//===- glsl_glass_backend_translator.cpp - Mesa customization of gla::BackEndTranslator -----===//
//
// LunarGLASS: An Open Modular Shader Compiler Architecture
// Copyright (C) 2010-2014 LunarG, Inc.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
// 
//     Redistributions of source code must retain the above copyright
//     notice, this list of conditions and the following disclaimer.
// 
//     Redistributions in binary form must reproduce the above
//     copyright notice, this list of conditions and the following
//     disclaimer in the documentation and/or other materials provided
//     with the distribution.
// 
//     Neither the name of LunarG Inc. nor the names of its
//     contributors may be used to endorse or promote products derived
//     from this software without specific prior written permission.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
// FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
// COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
// INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
// BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
// LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
// ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
//===----------------------------------------------------------------------===//
//
// Author: LunarG
//
// Customization of gla::BackEndTranslator for Mesa
//
//===----------------------------------------------------------------------===//

#ifdef USE_LUNARGLASS

// LunarGLASS includes
#include "Core/Revision.h"
#include "Core/Exceptions.h"
#include "Core/Util.h"
#include "Core/BottomIR.h"
#include "Core/Backend.h"
#include "Core/PrivateManager.h"
#include "Core/Options.h"
#include "Core/metadata.h"
#include "Core/Util.h"
#include "glsl_glass_backend_translator.h"

// Mesa includes
#include "main/shaderobj.h"
#include "ir.h"
#include "glsl_parser_extras.h"

// LLVM includes
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/InstIterator.h"
#include "llvm/Support/raw_ostream.h"

// Private includes
#include <map>

void emit_function(_mesa_glsl_parse_state *state, ir_function *f);

// Anonymous namespace for function local things
namespace {
   using namespace llvm;

   // **** Borrowed & modified from LunarGlass GLSL backend: ****
   class MetaType {
   public:
      MetaType() : precision(gla::EMpNone),
                   typeLayout(gla::EMtlNone),
                   interpMethod(gla::EIMNone),
                   interpLocation(gla::EILFragment),
                   qualifier(gla::EVQNone),
                   location(-1),
                   matrix(false), notSigned(false), block(false), mdAggregate(0), mdSampler(0) { }
      std::string                 name;
      gla::EMdPrecision           precision;
      gla::EMdTypeLayout          typeLayout;
      gla::EInterpolationMethod   interpMethod;
      gla::EInterpolationLocation interpLocation;
      gla::EVariableQualifier     qualifier;
      int                         location;
      bool                        matrix;
      bool                        notSigned;
      bool                        block;
      const llvm::MDNode*         mdAggregate;
      const llvm::MDNode*         mdSampler;
   };

   // **** Borrowed & modified from LunarGlass GLSL backend original: ****
   // Process the mdNode, decoding all type information and emitting qualifiers.
   // Returning false means there was a problem.
   bool decodeMdTypesEmitMdQualifiers(bool ioRoot, const llvm::MDNode* mdNode, const llvm::Type*& type, bool arrayChild, MetaType& metaType)
   {
      using namespace gla;

      if (ioRoot) {
         EMdInputOutput ioKind;
         llvm::Type* proxyType;
         int interpMode;
         if (! CrackIOMd(mdNode, metaType.name, ioKind, proxyType, metaType.typeLayout,
                         metaType.precision, metaType.location, metaType.mdSampler, metaType.mdAggregate, interpMode)) {
            return false;
         }

         metaType.block =
            ioKind == EMioUniformBlockMember ||
            ioKind == EMioBufferBlockMember  ||
            ioKind == EMioPipeOutBlock       ||
            ioKind == EMioPipeInBlock;

         if (type == 0)
            type = proxyType;

         // emit interpolation qualifier, if appropriate
         switch (ioKind) {
         case EMioPipeIn:   metaType.qualifier = EVQInput;   break;
         case EMioPipeOut:  metaType.qualifier = EVQOutput;  break;
         default:           metaType.qualifier = EVQUndef;   break;
         }
         if (metaType.qualifier != EVQUndef)
            CrackInterpolationMode(interpMode, metaType.interpMethod, metaType.interpLocation);
      } else {
         if (! CrackAggregateMd(mdNode, metaType.name, metaType.typeLayout,
                                metaType.precision, metaType.location, metaType.mdSampler))
            return false;
         metaType.mdAggregate = mdNode;
      }

      metaType.matrix = metaType.typeLayout == EMtlRowMajorMatrix || metaType.typeLayout == EMtlColMajorMatrix;
      metaType.notSigned = metaType.typeLayout == EMtlUnsigned;

      // LunarG TODO: ...
      // if (! arrayChild)
         // emitGlaLayout(out, typeLayout, metaType.location);

      return true;
   }

   // **** Borrowed from LunarGlass GLSL Backend
   gla::EVariableQualifier MapGlaAddressSpace(const llvm::Value* value)
   {
      using namespace gla;
      if (const llvm::PointerType* pointer = llvm::dyn_cast<llvm::PointerType>(value->getType())) {
          switch (pointer->getAddressSpace()) {
          case ResourceAddressSpace:
              return EVQUniform;
          case GlobalAddressSpace:
              return EVQGlobal;
          default:
              if (pointer->getAddressSpace() >= ConstantAddressSpaceBase)
                  return EVQUniform;

              UnsupportedFunctionality("Address Space in Bottom IR: ", pointer->getAddressSpace());
              break;
          }
      }

      if (llvm::isa<llvm::Instruction>(value))
          return EVQTemporary;

      // Check for an undef before a constant (since Undef is a
      // subclass of Constant)
      if (AreAllUndefined(value)) {
          return EVQUndef;
      }

      if (llvm::isa<llvm::Constant>(value)) {
          return EVQConstant;
      }

      return EVQTemporary;
   }


   // **** Borrowed & modified from LunarGlass GLSL Backend
   // Whether the given intrinsic's specified operand is the same as the passed
   // value, and its type is a vector.
   bool IsSameSource(const llvm::Value *source, const llvm::Value *prevSource)
   {
       return source && prevSource == source &&
          source->getType()->getTypeID() == llvm::Type::VectorTyID;
   }


   //
   // **** Borrowed from LunarGlass GLSL Backend
   // Figure out how many I/O slots 'type' would fill up.
   //
   int CountSlots(const llvm::Type* type)
   {
      if (type->getTypeID() == llvm::Type::VectorTyID)
         return 1;
      else if (type->getTypeID() == llvm::Type::ArrayTyID) {
         const llvm::ArrayType* arrayType = llvm::dyn_cast<const llvm::ArrayType>(type);
         return (int)arrayType->getNumElements() * CountSlots(arrayType->getContainedType(0));
      } else if (type->getTypeID() == llvm::Type::StructTyID) {
         const llvm::StructType* structType = llvm::dyn_cast<const llvm::StructType>(type);
         int slots = 0;
         for (unsigned int f = 0; f < structType->getStructNumElements(); ++f)
            slots += CountSlots(structType->getContainedType(f));

         return slots;
      }

      return 1;
   }

   static const int NumSamplerTypes     = 2;
   static const int NumSamplerBaseTypes = 3;
   static const int NumSamplerDims      = 6;
   static const glsl_type* SamplerTypes[NumSamplerTypes][NumSamplerBaseTypes][NumSamplerDims][true+1][true+1];

   // C++ before 11 doesn't have a static_assert.  This is a hack to provide
   // one.  If/when this code is ever migrated to C++11, remove this, and use
   // the language's native static_assert.
   template <bool> struct ersatz_static_assert;
   template <> struct ersatz_static_assert<true> { bool used; };

   // If this fails during compilation, it means LunarGlass has added new sampler types
   // We must react to.  Also, see comment above ersatz_static_assert.
   ersatz_static_assert<NumSamplerTypes     == gla::EMsCount &&
                        NumSamplerBaseTypes == gla::EMsbCount &&
                        NumSamplerDims      == gla::EMsdCount> SamplerTypeMismatchWithLunarGlass;

   void InitSamplerTypes()
   {
      using namespace gla;
      memset(SamplerTypes, 0, sizeof(SamplerTypes));

      SamplerTypeMismatchWithLunarGlass.used = true; // just to quiet a compiler warning about unused vars

      // LunarG TODO: verify these, and fill out all types
      // LunarG TODO: need EMsImage types in addition to EMsTexture

      //           TYPE        BASETYPE   DIM        SHADOW  ARRAY    GLSL_SAMPLER_TYPE
      SamplerTypes[EMsTexture][EMsbFloat][EMsd1D]    [false][false] = glsl_type::sampler1D_type;
      SamplerTypes[EMsTexture][EMsbFloat][EMsd2D]    [false][false] = glsl_type::sampler2D_type;
      SamplerTypes[EMsTexture][EMsbFloat][EMsd3D]    [false][false] = glsl_type::sampler3D_type;
      SamplerTypes[EMsTexture][EMsbFloat][EMsdCube]  [false][false] = glsl_type::samplerCube_type;
      SamplerTypes[EMsTexture][EMsbFloat][EMsdRect]  [false][false] = glsl_type::sampler2DRect_type;
      SamplerTypes[EMsTexture][EMsbFloat][EMsdBuffer][false][false] = glsl_type::samplerBuffer_type;

      SamplerTypes[EMsTexture][EMsbInt]  [EMsd1D]    [false][false] = glsl_type::isampler1D_type;
      SamplerTypes[EMsTexture][EMsbInt]  [EMsd2D]    [false][false] = glsl_type::isampler2D_type;
      SamplerTypes[EMsTexture][EMsbInt]  [EMsd3D]    [false][false] = glsl_type::isampler3D_type;
      SamplerTypes[EMsTexture][EMsbInt]  [EMsdCube]  [false][false] = glsl_type::isamplerCube_type;
      SamplerTypes[EMsTexture][EMsbInt]  [EMsdRect]  [false][false] = glsl_type::isampler2DRect_type;
      SamplerTypes[EMsTexture][EMsbInt]  [EMsdBuffer][false][false] = glsl_type::isamplerBuffer_type;

      SamplerTypes[EMsTexture][EMsbUint] [EMsd1D]    [false][false] = glsl_type::usampler1D_type;
      SamplerTypes[EMsTexture][EMsbUint] [EMsd2D]    [false][false] = glsl_type::usampler2D_type;
      SamplerTypes[EMsTexture][EMsbUint] [EMsd3D]    [false][false] = glsl_type::usampler3D_type;
      SamplerTypes[EMsTexture][EMsbUint] [EMsdCube]  [false][false] = glsl_type::usamplerCube_type;
      SamplerTypes[EMsTexture][EMsbUint] [EMsdRect]  [false][false] = glsl_type::usampler2DRect_type;
      SamplerTypes[EMsTexture][EMsbUint] [EMsdBuffer][false][false] = glsl_type::usamplerBuffer_type;

      SamplerTypes[EMsTexture][EMsbFloat][EMsd1D]    [true] [false] = glsl_type::sampler1DShadow_type;
      SamplerTypes[EMsTexture][EMsbFloat][EMsd2D]    [true] [false] = glsl_type::sampler2DShadow_type;
      SamplerTypes[EMsTexture][EMsbFloat][EMsdCube]  [true] [false] = glsl_type::samplerCubeShadow_type;
      SamplerTypes[EMsTexture][EMsbFloat][EMsdRect]  [true] [false] = glsl_type::sampler2DRectShadow_type;

      SamplerTypes[EMsTexture][EMsbFloat][EMsd1D]    [true] [true]  = glsl_type::sampler1DArrayShadow_type;
      SamplerTypes[EMsTexture][EMsbFloat][EMsd2D]    [true] [true]  = glsl_type::sampler2DArrayShadow_type;
      SamplerTypes[EMsTexture][EMsbFloat][EMsdCube]  [true] [true]  = glsl_type::samplerCubeArrayShadow_type;

      SamplerTypes[EMsTexture][EMsbInt][EMsd1D]      [false][true]  = glsl_type::isampler1DArray_type;
      SamplerTypes[EMsTexture][EMsbInt][EMsd2D]      [false][true]  = glsl_type::isampler2DArray_type;
      SamplerTypes[EMsTexture][EMsbInt][EMsdCube]    [false][true]  = glsl_type::isamplerCubeArray_type;

      SamplerTypes[EMsTexture][EMsbUint][EMsd1D]     [false][true]  = glsl_type::usampler1DArray_type;
      SamplerTypes[EMsTexture][EMsbUint][EMsd2D]     [false][true]  = glsl_type::usampler2DArray_type;
      SamplerTypes[EMsTexture][EMsbUint][EMsdCube]   [false][true]  = glsl_type::usamplerCubeArray_type;

      SamplerTypes[EMsTexture][EMsbFloat][EMsd1D]    [false][true]  = glsl_type::sampler1DArray_type;
      SamplerTypes[EMsTexture][EMsbFloat][EMsd2D]    [false][true]  = glsl_type::sampler2DArray_type;
      SamplerTypes[EMsTexture][EMsbFloat][EMsdCube]  [false][true]  = glsl_type::samplerCubeArray_type;
   }

   /**
    * -----------------------------------------------------------------------------
    * Convert sampler type info from LunarGlass Metadata to glsl_type
    * -----------------------------------------------------------------------------
    */
   const glsl_type* GetSamplerType(gla::EMdSampler samplerType,
                                   gla::EMdSamplerBaseType baseType,
                                   gla::EMdSamplerDim samplerDim,
                                   bool isShadow, bool isArray)
   {
      // Bounds check so we don't overflow array!  The bools can't overflow.
      if (samplerType >= NumSamplerTypes     ||
          baseType    >= NumSamplerBaseTypes ||
          samplerDim  >= NumSamplerDims) {
         assert(0 && "LunarG TODO: Handle error");  // LunarG TODO: handle internal error.  should never happen.
         return 0;
      }

      return SamplerTypes[samplerType][baseType][samplerDim][isShadow][isArray];
   }


   /**
    * -----------------------------------------------------------------------------
    * Convert interpolation qualifiers to HIR version
    * -----------------------------------------------------------------------------
    */
   glsl_interp_qualifier InterpolationQualifierToIR(gla::EInterpolationMethod im)
   {
      switch (im) {
      case gla::EIMSmooth:        return INTERP_QUALIFIER_SMOOTH;
      case gla::EIMNoperspective: return INTERP_QUALIFIER_NOPERSPECTIVE;
      case gla::EIMNone:          // fall through...
      default:                    return INTERP_QUALIFIER_FLAT;
      }
   }

   /**
    * -----------------------------------------------------------------------------
    * Convert type layout to HIR version
    * -----------------------------------------------------------------------------
    */
    glsl_interface_packing TypeLayoutToIR(gla::EMdTypeLayout layout)
    {
       switch (layout) {
       case gla::EMtlShared: return GLSL_INTERFACE_PACKING_SHARED;
       case gla::EMtlPacked: return GLSL_INTERFACE_PACKING_PACKED;
       case gla::EMtlStd430: assert(0 && "No HIR support yet");
       case gla::EMtlStd140: // fall through...
       default:              return GLSL_INTERFACE_PACKING_STD140;
       }
    }


   /**
    * -----------------------------------------------------------------------------
    * Convert block mode
    * -----------------------------------------------------------------------------
    */
    ir_variable_mode VariableQualifierToIR(gla::EVariableQualifier qualifier)
    {
       // LunarG TODO: ir_var_system_value - figure out what must use that.

       switch (qualifier) {
       case gla::EVQUniform:   return ir_var_uniform;
       case gla::EVQInput:     return ir_var_shader_in;
       case gla::EVQOutput:    return ir_var_shader_out;
       case gla::EVQConstant:  return ir_var_const_in;
       case gla::EVQTemporary: return ir_var_temporary;
       case gla::EVQGlobal:    return ir_var_auto;
       default:                return ir_var_auto;
       }
    }


   /**
    * -----------------------------------------------------------------------------
    * Deduce whether a metadata node IO metadata, or aggregate.  Does not
    * grok other MD types.
    * -----------------------------------------------------------------------------
    */
    inline bool isIoMd(const llvm::MDNode* mdNode)
    {
       return mdNode &&
              mdNode->getNumOperands() > 3 &&
              llvm::dyn_cast<const llvm::Value>(mdNode->getOperand(2)) &&
              llvm::dyn_cast<const llvm::MDNode>(mdNode->getOperand(3));
    }

    inline bool isIoAggregateMd(const llvm::MDNode* mdNode)
    {
       return isIoMd(mdNode) && mdNode->getNumOperands() > 4 &&
              llvm::dyn_cast<const llvm::MDNode>(mdNode->getOperand(4));
    }

} // anonymous namespace


namespace gla {

/**
 * -----------------------------------------------------------------------------
 * Clean up
 * -----------------------------------------------------------------------------
 */
MesaGlassTranslator::~MesaGlassTranslator()
{
   while (!toDelete.empty()) {
      delete toDelete.back();
      toDelete.pop_back();
   }
}

// **** Borrowed from LunarGlass GLSL backend: ****
// 'gep' is potentially a gep, either an instruction or a constantExpr.
// See which one, if any, and return it.
// Return 0 if not a gep.
const llvm::GetElementPtrInst* MesaGlassTranslator::getGepAsInst(const llvm::Value* gep)
{
   const llvm::GetElementPtrInst* gepInst = llvm::dyn_cast<const llvm::GetElementPtrInst>(gep);
   if (gepInst)
      return gepInst;

   // LLVM isn't always const correct.  I believe getAsInstruction() doesn't
   // modify the original, so this is safe.
   llvm::ConstantExpr *constantGep = const_cast<llvm::ConstantExpr*>(llvm::dyn_cast<llvm::ConstantExpr>(gep));

   if (constantGep) {
      const llvm::Instruction *instruction = constantGep->getAsInstruction();
      toDelete.push_back(instruction);
      gepInst = llvm::dyn_cast<const llvm::GetElementPtrInst>(instruction);
   }

   return gepInst;
}


/**
 * -----------------------------------------------------------------------------
 * initialize translation state
 * -----------------------------------------------------------------------------
 */
void MesaGlassTranslator::start(llvm::Module& module)
{
   countReferences(module);
}


/**
 * -----------------------------------------------------------------------------
 * initialize translation state
 * -----------------------------------------------------------------------------
 */
void MesaGlassTranslator::initializeTranslation(gl_context *ctx, _mesa_glsl_parse_state *state, gl_shader *shader)
{
   InitSamplerTypes();  // LunarG TODO: don't do this for each translation!

   setContext(ctx);
   setParseState(state);
   setShader(shader);

   ralloc_free(shader->ir);
   shader->ir = new(shader) exec_list;

   _mesa_glsl_initialize_types(state);
   _mesa_glsl_initialize_variables(shader->ir, state);

   // Seed our globalDeclMap with whatever _mesa_glsl_initialize_variables did.
   seedGlobalDeclMap();

   state->symbols->separate_function_namespace = state->language_version == 110;

   state->current_function = NULL;

   state->toplevel_ir = shader->ir;

   instructionStack.push_back(shader->ir);

   // LunarG TODO:
   state->gs_input_prim_type_specified = false;
   state->cs_input_local_size_specified = false;

   /* Section 4.2 of the GLSL 1.20 specification states:
    * "The built-in functions are scoped in a scope outside the global scope
    *  users declare global variables in.  That is, a shader's global scope,
    *  available for user-defined functions and global variables, is nested
    *  inside the scope containing the built-in functions."
    *
    * Since built-in functions like ftransform() access built-in variables,
    * it follows that those must be in the outer scope as well.
    *
    * We push scope here to create this nesting effect...but don't pop.
    * This way, a shader's globals are still in the symbol table for use
    * by the linker.
    */
   state->symbols->push_scope();

   // Initialize builtin functions we might use (tx sampling, etc)
   _mesa_glsl_initialize_builtin_functions();
}


/**
 * -----------------------------------------------------------------------------
 * finalize translation state
 * -----------------------------------------------------------------------------
 */
void MesaGlassTranslator::finalizeTranslation()
{
   // Currently, nothing to do here...
}


/**
 * -----------------------------------------------------------------------------
 * Seed the global declaration map from the initial declarations
 * from _mesa_glsl_initialize_variables.
 * -----------------------------------------------------------------------------
 */
void MesaGlassTranslator::seedGlobalDeclMap()
{
   foreach_list_safe(node, shader->ir) {
      ir_variable *const var = ((ir_instruction *) node)->as_variable();

      if (var)
         globalDeclMap[var->name] = var;
   }
}


/**
 * -----------------------------------------------------------------------------
 * We count the references of each lvalue, to know when to generate
 * assignments and when to directly create tree nodes
 * -----------------------------------------------------------------------------
 */
void MesaGlassTranslator::countReferences(const llvm::Module& module)
{
   using namespace llvm;

   // Iterate over all functions in module
   for (Module::const_iterator func = module.begin(); func != module.end(); ++func) {
   
      // Iterate over all instructions in all blocks in function
      for (const_inst_iterator inst = inst_begin(func), fnEnd = inst_end(func);
           inst != fnEnd; ++inst) {

         // Keep track of which local values should be signed ints
         switch ((&*inst)->getOpcode()) {
         case llvm::Instruction::ZExt:   // fall through...
         case llvm::Instruction::FPToSI: sintValues.insert(&*inst);                  break;
         case llvm::Instruction::SIToFP: sintValues.insert((&*inst)->getOperand(0)); break;
         case llvm::Instruction::SRem:   // fall through...
         case llvm::Instruction::SDiv:
            sintValues.insert((&*inst));
            sintValues.insert((&*inst)->getOperand(0));
            sintValues.insert((&*inst)->getOperand(1));
            break;
         default: break;
         }

         // Iterate over all operands in the instruction
         for (User::const_op_iterator op = inst->op_begin(), opEnd = inst->op_end();
              op != opEnd; ++op) {
            // Count each use
            refCountMap[*op]++;
         }
      }
   }

   // For debugging: change to true to dump ref counts
   static const bool debugReferenceCounts = false;

   if (debugReferenceCounts) {
      FILE* out = stderr;
      std::string name;
      llvm::raw_string_ostream nameStream(name);

      fprintf(out, "RValue Ref Counts\n%-10s : %3s\n",
              "RValue", "#");

      for (tRefCountMap::const_iterator ref = refCountMap.begin();
           ref != refCountMap.end(); ++ref) {
         name.clear();
         // ref->first->printAsOperand(nameStream);
         fprintf(out, "%-10s : %3d\n", ref->first->getName().str().c_str(), ref->second);
      }
   }
}


/**
 * -----------------------------------------------------------------------------
 * Return true if we have a valueMap entry for this LLVM value
 * -----------------------------------------------------------------------------
 */
inline bool MesaGlassTranslator::valueEntryExists(const llvm::Value* value) const
{
   return valueMap.find(value) != valueMap.end();
}


/**
 * -----------------------------------------------------------------------------
 * Return ref count of an rvalue
 * -----------------------------------------------------------------------------
 */

inline unsigned MesaGlassTranslator::getRefCount(const llvm::Value* value) const
{
   const tRefCountMap::const_iterator found = refCountMap.find(value);

   // Return 0 if not found, or refcount if found
   return (found == refCountMap.end()) ? 0 : found->second;
}


/**
 * -----------------------------------------------------------------------------
 * Encapsulate creation of variables
 * -----------------------------------------------------------------------------
 */
inline ir_variable*
MesaGlassTranslator::newIRVariable(const glsl_type* type, const char* name,
                                   int mode, bool declare)
{
   // If we have a global decl, use that.  HIR form expects use of the same
   // ir_variable node as the global decl used.
   const tGlobalDeclMap::const_iterator globalDecl = globalDeclMap.find(name);
   if (globalDecl != globalDeclMap.end())
      return globalDecl->second;

   // ir_variable constructor strdups the name, so it's OK if it points to something ephemeral
   ir_variable* var = new(shader) ir_variable(type, name, ir_variable_mode(mode));
   var->data.used = true;

   // LunarG TODO: Can we stash these away in a map and return existing ones, to make
   // our memory footprint smaller?

   // LunarG TODO: handle interpolation modes, locations, precisions, etc etc etc
   //              poke them into the right bits in the ir_variable
   // var->data.centroid =
   // var->data.sample =
   // var->data.q.*
   // var->data.invariant
   // var->data.binding
   // var->data.varying
   // var->data.how_declared ?

   // LunarG TODO: also set for consts and vertex attrs per comment ir.h:153
   const bool readOnly = (mode == ir_var_uniform);
   if (readOnly)
      var->data.read_only = true;

   // LunarG TODO: To avoid redeclaring builtins, we have a hack to look at the name.
   // Something better should be done here.  Because of short circuit evaluation,
   // this is safe against name strings that aren't 3 characters long.
   const bool isBuiltin = name && name[0] == 'g' && name[1] == 'l' && name[2] == '_';

   // Declare the variable by adding it as either a global or local to the instruction list
   const bool global = (mode == ir_var_uniform   ||
                        mode == ir_var_shader_in ||
                        mode == ir_var_shader_out);

   if (!isBuiltin && (!global || declare))
      addIRInstruction(var, global);

   // Declare globals in the global decl map
   if (declare)
      globalDeclMap[name] = var;

   return var;
}

inline ir_variable*
MesaGlassTranslator::newIRVariable(const glsl_type* type, const std::string& name,
                                   int mode, bool declare)
{
   return newIRVariable(type, name.c_str(), mode, declare);
}

inline ir_variable*
MesaGlassTranslator::newIRVariable(const llvm::Type* type,
                                   const llvm::Value* value,
                                   const char* name,
                                   int mode, bool declare)
{
   return newIRVariable(llvmTypeToHirType(type, 0, value), name, mode, declare);
}

inline ir_variable*
MesaGlassTranslator::newIRVariable(const llvm::Type* type,
                                   const llvm::Value* value, 
                                   const std::string& name,
                                   int mode, bool declare)
{
   return newIRVariable(llvmTypeToHirType(type, 0, value), name.c_str(), mode, declare);
}


/**
 * -----------------------------------------------------------------------------
 * Encapsulate creation of variable dereference
 * -----------------------------------------------------------------------------
 */
inline ir_dereference*
MesaGlassTranslator::newIRVariableDeref(const glsl_type* type, const char* name, int mode, bool declare)
{
   return new(shader) ir_dereference_variable(newIRVariable(type, name, mode, declare));
}

inline ir_dereference*
MesaGlassTranslator::newIRVariableDeref(const glsl_type* type, const std::string& name, int mode, bool declare)
{
   return newIRVariableDeref(type, name.c_str(), mode, declare);
}

inline ir_dereference*
MesaGlassTranslator::newIRVariableDeref(const llvm::Type* type,
                                        const llvm::Value* value, 
                                        const char* name, int mode,
                                        bool declare)
{
   return newIRVariableDeref(llvmTypeToHirType(type, 0, value), name, mode, declare);
}

inline ir_dereference*
MesaGlassTranslator::newIRVariableDeref(const llvm::Type* type,
                                        const llvm::Value* value,
                                        const std::string& name, int mode,
                                        bool declare)
{
   return newIRVariableDeref(llvmTypeToHirType(type, 0, value), name.c_str(), mode, declare);
}


/**
 * -----------------------------------------------------------------------------
 * Emit IR texture intrinsics
 *   (declare (uniform ) sampler2D Diffuse)
 *   (call texture (var_ref texture_retval)  ((var_ref Diffuse) (var_ref VTexcoord) ))
 * -----------------------------------------------------------------------------
 */
inline void MesaGlassTranslator::emitIRTexture(const llvm::IntrinsicInst* llvmInst, bool gather)
{
   const unsigned texFlags        = GetConstantInt(llvmInst->getOperand(GetTextureOpIndex(ETOFlag)));
   const llvm::Value* samplerType = llvmInst->getOperand(0);

   std::string txName;
   txName.reserve(20);

   // Original style shadowing returns vec4 while 2nd generation returns float,
   // so, have to stick to old-style for those cases.
   const bool forceOldStyle = IsVector(llvmInst->getType()) && (texFlags & ETFShadow) && ((texFlags & ETFGather) == 0);

   if (manager->getVersion() >= 130 && !forceOldStyle) {
      if (texFlags & ETFFetch)
         txName = "texelFetch";
      else
         txName = "texture";
   } else {
      if (texFlags & ETFShadow)
         txName = "shadow";
      else
         txName = "texture";

      const int sampler = GetConstantInt(samplerType);

      switch (sampler) {
      case ESampler1D:        txName += "1D";   break;
      case ESampler2D:        txName += "2D";   break;
      case ESampler3D:        txName += "3D";   break;
      case ESamplerCube:      txName += "Cube"; break;
      case ESampler2DRect:    txName += "Rect"; break;
      default:
         assert(0 && "LunarG TODO: Handle error"); // LunarG TODO: handle error
         break;
      }
   }

   if (texFlags & ETFProjected)
      txName += "Proj";

   // This is OK on the stack, because the ir_call constructor will move out of it.
   exec_list parameters;

   // Sampler operand
   parameters.push_tail(getIRValue(llvmInst->getOperand(GetTextureOpIndex(ETOSamplerLoc))));

   // Coordinate
   parameters.push_tail(getIRValue(llvmInst->getOperand(GetTextureOpIndex(ETOCoord))));

   // RefZ
   if (texFlags & ETFRefZArg) {
      parameters.push_tail(getIRValue(llvmInst->getOperand(GetTextureOpIndex(ETORefZ))));
   }

   // LOD
   if (texFlags & ETFLod) {
      txName += "Lod";
      parameters.push_tail(getIRValue(llvmInst->getOperand(GetTextureOpIndex(ETOBiasLod))));
   }

   // dPdX/dPdY
   if (IsGradientTexInst(llvmInst)) {
      txName += "Grad";
      parameters.push_tail(getIRValue(llvmInst->getOperand(GetTextureOpIndex(ETODPdx))));
      parameters.push_tail(getIRValue(llvmInst->getOperand(GetTextureOpIndex(ETODPdy))));
   }

   if (texFlags & ETFGather) {
      assert(0 && "LunarG TODO: Handle gather"); // LunarG TODO;
      txName += "Gather";
   }

   // Offsets
   // LunarG TODO: ...
   if (texFlags & ETFOffsetArg) {
      assert(0 && "LunarG TODO: Handle offset arg"); // LunarG TODO;

      if (texFlags & ETFOffsets)
         txName += "Offsets";
      else
         txName += "Offset";
      // llvmInst->getOperand(GetTextureOpIndex(ETOOffset) + i)
   }

   // BiasLOD
   if (((texFlags & ETFBiasLodArg) != 0 && (texFlags & ETFLod) == 0) ||
       (texFlags & ETFComponentArg)) {
      parameters.push_tail(getIRValue(llvmInst->getOperand(GetTextureOpIndex(ETOBiasLod))));
   }

   // Find the right function signature to call
   // This sets state->uses_builtin_functions
   ir_function_signature *sig =
      _mesa_glsl_find_builtin_function(state, txName.c_str(), &parameters);

   if (sig == 0) {
      assert(0 && "LunarG TODO: Handle error");
      return;
   }

   const std::string retName = std::string(txName) + "_retval";
   ir_dereference* dest = newIRVariableDeref(llvmInst->getType(), llvmInst, retName, ir_var_auto);

   // LunarG TODO: what's up with this vs ir_texture?  old stack makes calls, but there's an
   // ir_texture.  ???

   ir_call *call = new(shader) ir_call(sig, dest->as_dereference_variable(), &parameters);

   // LunarG TODO: Should we insert a prototype?

   addIRInstruction(llvmInst, call);
}


/**
 * -----------------------------------------------------------------------------
 * Add a plain (non-conditional) discard
 * -----------------------------------------------------------------------------
 */
void MesaGlassTranslator::addDiscard()
{
   addIRInstruction(new(shader) ir_discard);
}


/**
 * -----------------------------------------------------------------------------
 * Translate structs
 * -----------------------------------------------------------------------------
 */
void addStructType(llvm::StringRef, const llvm::Type*)
{
}


/**
 * -----------------------------------------------------------------------------
 * Translate globals
 * -----------------------------------------------------------------------------
 */
void MesaGlassTranslator::addGlobal(const llvm::GlobalVariable* global)
{
   const gla::EVariableQualifier qualifier = MapGlaAddressSpace(global);

   switch (qualifier) {
   case gla::EVQUniform:
   case gla::EVQGlobal:
   case gla::EVQTemporary: break;
   default: return;  // nothing to do for these
   }

   llvm::Type* type;
   if (const llvm::PointerType* pointer = llvm::dyn_cast<llvm::PointerType>(global->getType()))
      type = pointer->getContainedType(0);
   else
      type = global->getType();

   const ir_variable_mode irMode   = VariableQualifierToIR(qualifier);
   const std::string&     name     = global->getName();
   ir_rvalue*             varDeref = newIRVariableDeref(type, 0, name, irMode, true);
   ir_variable*           var      = varDeref->as_dereference_variable()->variable_referenced();

   // Remember the ir_variable_mode for this declaration
   globalVarModeMap[name] = irMode;

   // LunarG TODO: declare global struct types (... how?)

   // add initializers to main prologue or variable (for uniforms)
   if (global->hasInitializer()) {
      var->data.has_initializer = true;
      ir_constant* constVal = newIRConstant(global->getInitializer());

      if (qualifier == gla::EVQUniform) {
         var->constant_value       = constVal;
         var->constant_initializer = constVal->clone(shader, 0);
         // Create uniform initializers
      } else {
         // Non-uniforms get explicit initialization
         prologue.push_back(new(shader) ir_assignment(varDeref, constVal));
      }
   }
}


/**
 * -----------------------------------------------------------------------------
 * Create global IR declarations for shaders ins, outs, and uniforms
 * -----------------------------------------------------------------------------
 */
void MesaGlassTranslator::addIoDeclaration(gla::EVariableQualifier qualifier,
                                           const llvm::MDNode* mdNode)
{
   const llvm::Type* mdType = mdNode->getOperand(2)->getType()->getContainedType(0);

   const llvm::StringRef& name = mdNode->getOperand(0)->getName();

   // Register name -> metadata mapping for this declaration
   typenameMdMap[name] = mdNode;

   const glsl_type*       irType    = llvmTypeToHirType(mdType, mdNode);
   const ir_variable_mode irVarMode = VariableQualifierToIR(qualifier);

   // Create IR declaration
   newIRVariable(irType, name, irVarMode, true);

   // Register interface block, if it is one.  We want to use the interface
   // name, not the type name, in this registration.
   if (irType->is_interface()) {
      if (!state->symbols->add_interface(irType->name, irType, irVarMode)) {
         assert(0 && "LunarG TODO: Handle error"); 
      }
   }
}


/**
 * -----------------------------------------------------------------------------
 * Translate function arguments
 * -----------------------------------------------------------------------------
 */
void MesaGlassTranslator::addArgument(const llvm::Value* value, bool last)
{
   assert(0 && "LunarG TODO: Untested as of yet");
   fnParameters.push_tail(getIRValue(value));
}


/**
 * -----------------------------------------------------------------------------
 * Reset any pending function state
 * -----------------------------------------------------------------------------
 */
void MesaGlassTranslator::resetFnTranslationState()
{
   // We don't free memory for the fn name, because someone else owns it
   fnName = 0;

   // Destroy any pending fn return type
   delete fnReturnType;
   fnReturnType = 0;

   // Destroy function signature
   if (fnSignature) {
      ralloc_free(fnSignature);
      fnSignature = 0;
   }

   // Destroy function
   if (fnFunction) {
      ralloc_free(fnFunction);
      fnFunction = 0;
   }

   // empty parameter list
   fnParameters.make_empty();
}


/**
 * -----------------------------------------------------------------------------
 * Convert struct types.  May call llvmTypeToHirType recursively.
 * -----------------------------------------------------------------------------
 */
const glsl_type*
MesaGlassTranslator::convertStructType(const llvm::Type*       structType,
                                       llvm::StringRef         name,
                                       const llvm::MDNode*     mdNode,
                                       gla::EMdTypeLayout      parentTypeLayout,
                                       gla::EVariableQualifier parentQualifier,
                                       bool                    isBlock)
{
   const int containedTypeCount = structType->getNumContainedTypes();

   // Allocate an array of glsl_struct_fields of size to hold all subtypes
   glsl_struct_field *const fields = ralloc_array(shader, glsl_struct_field, containedTypeCount);
   
   char anonFieldName[20]; // enough for "anon_" + 9 digits

   for (int index = 0; index < containedTypeCount; ++index) {
      const llvm::MDNode* subMdAggregate =
         mdNode ? llvm::dyn_cast<llvm::MDNode>(mdNode->getOperand(GetAggregateMdSubAggregateOp(index))) : 0;

      const char* subName;
      const llvm::Type* containedType  = structType->getContainedType(index);

      // If there's metadata, use that field name.  Else, make up a field name.
      if (mdNode) {
         subName = mdNode->getOperand(GetAggregateMdNameOp(index))->getName().str().c_str();
      } else {
         snprintf(anonFieldName, sizeof(anonFieldName), "anon_%d", index);
         subName = anonFieldName;
      }

      // LunarG TODO: set arrayChild, etc properly
      MetaType metaType;

      fields[index].name          = ralloc_strdup(shader, subName);
      fields[index].type          = llvmTypeToHirType(containedType, subMdAggregate);

      if (subMdAggregate) {
         decodeMdTypesEmitMdQualifiers(false, subMdAggregate, containedType, false, metaType);

         fields[index].location      = metaType.location;
         fields[index].interpolation = InterpolationQualifierToIR(metaType.interpMethod);
         fields[index].centroid      = metaType.interpLocation == EILCentroid;
         fields[index].sample        = metaType.mdSampler != 0;
         fields[index].row_major     = metaType.typeLayout == EMtlRowMajorMatrix;
      }

      // LunarG TODO: handle array size
      // LunarG TODO: handle qualifiers
   }

   if (isBlock) {
      return glsl_type::get_interface_instance(fields, containedTypeCount,
                                               TypeLayoutToIR(parentTypeLayout),
                                               ralloc_strdup(shader, name.str().c_str()));
   } else {
      return glsl_type::get_record_instance(fields, containedTypeCount,
                                            ralloc_strdup(shader, name.str().c_str()));
   }
}


/**
 * -----------------------------------------------------------------------------
 * Convert an LLVM type to an HIR type
 * -----------------------------------------------------------------------------
 */
const glsl_type*
MesaGlassTranslator::llvmTypeToHirType(const llvm::Type*   type,
                                       const llvm::MDNode* mdNode,
                                       const llvm::Value*  llvmValue)
{
   // Find any aggregate metadata we may have stored away in a prior declaration
   const tMDMap::const_iterator aggMdFromMap = typeMdAggregateMap.find(type);
   const llvm::MDNode* mdAggNode = (aggMdFromMap != typeMdAggregateMap.end()) ? aggMdFromMap->second : 0;

   if (!mdNode)
      mdNode = mdAggNode;

   const tTypeData typeData(type, mdNode);

   // See if we've already converted this type.  If so, use the one we did already.
   const tTypeMap::const_iterator foundType = typeMap.find(typeData);

   if (foundType != typeMap.end())
      return foundType->second;
   
   // Otherwise, we must convert it.
   const glsl_type* return_type = glsl_type::error_type;

   const EMdTypeLayout mdType = mdNode ? GetMdTypeLayout(mdNode) : gla::EMtlNone;

   const bool signedInt = (llvmValue && sintValues.find(llvmValue) != sintValues.end()) ||
      (mdType != EMtlUnsigned);

   // LunarG TODO: handle precision, etc
   switch (type->getTypeID())
   {
   case llvm::Type::VectorTyID:
      {
         const llvm::VectorType *vectorType = llvm::dyn_cast<llvm::VectorType>(type);
         assert(vectorType);

         glsl_base_type glslBaseType;

         if (type->getContainedType(0) == type->getFloatTy(type->getContext()))
            glslBaseType = GLSL_TYPE_FLOAT;
         else if (gla::IsBoolean(type->getContainedType(0)))
            glslBaseType = GLSL_TYPE_BOOL;
         else if (gla::IsInteger(type->getContainedType(0)))
            glslBaseType = signedInt ? GLSL_TYPE_INT : GLSL_TYPE_UINT;
         else 
            glslBaseType = GLSL_TYPE_VOID;

         // LunarG TODO: Ugly const_cast necessary here at the moment
         const int componentCount = gla::GetComponentCount(const_cast<llvm::Type*>(type));
         return_type = glsl_type::get_instance(glslBaseType, componentCount, 1);
         break;
      }
 
   case llvm::Type::ArrayTyID:
      {
         const llvm::ArrayType* arrayType     = llvm::dyn_cast<const llvm::ArrayType>(type);
         const llvm::Type*      containedType = arrayType->getContainedType(0);
         const int              arraySize     = arrayType->getNumElements();
         assert(arrayType);

         const bool isMat = (mdType == EMtlRowMajorMatrix || mdType == EMtlColMajorMatrix);

         // Reconstruct matrix types from arrays of vectors, per metadata hints
         if (isMat) {
            const llvm::VectorType* vectorType = llvm::dyn_cast<llvm::VectorType>(containedType);

            // For arrays of arrays of vectors, we want one more level of dereference.
            // LunarG TODO: should GetMdTypeLayout(mdNode) really be telling us "mat" here?
            if (vectorType)
               return glslMatType(arraySize, vectorType->getNumElements());
         }

         // Metadata type will be the same
         const glsl_type* containedIRType = llvmTypeToHirType(containedType, mdNode, llvmValue);

         return_type = glsl_type::get_array_instance(containedIRType, arraySize);
         break;
      }

   case llvm::Type::StructTyID:
      {
         const llvm::StructType* structType = llvm::dyn_cast<const llvm::StructType>(type);
         assert(structType);

         const llvm::StringRef structName = structType->isLiteral() ? "" : structType->getName();

         // Check for a top level uniform/input/output MD with an aggregate MD hanging off it
         // LunarG TODO: set arrayChild properly!
         MetaType metaType;
         if (mdNode) {
             decodeMdTypesEmitMdQualifiers(isIoMd(mdNode), mdNode, type, false, metaType);

             // Convert IO metadata to aggregate metadata if needed.
             if (isIoAggregateMd(mdNode))
                mdNode = llvm::dyn_cast<const llvm::MDNode>(mdNode->getOperand(4));

             // track the mapping between the type and it's aggregate metadata
             typeMdAggregateMap[structType] = mdNode;
         }

         return_type = convertStructType(structType, structName, mdNode,
                                         metaType.typeLayout,
                                         metaType.qualifier,
                                         metaType.block);
         break;
      }

   case llvm::Type::PointerTyID:
      // LunarG TODO:
      assert(0 && "unimplemented");
      break;

   case llvm::Type::IntegerTyID:
      {
         // Sampler type conversions
         if (mdType == EMtlSampler) {
            EMdSampler         mdSampler;
            llvm::Type*        mdType;
            EMdSamplerDim      mdSamplerDim;
            bool               isArray;
            bool               isShadow;
            EMdSamplerBaseType mdBaseType;

            // Handle aggregate member form
            const int typeNodePos = isIoMd(mdNode) ? 3 : 1;

            const llvm::MDNode* typeMdNode    = llvm::dyn_cast<const llvm::MDNode>(mdNode->getOperand(typeNodePos));
            assert(typeMdNode);
            const llvm::MDNode* samplerMdNode = llvm::dyn_cast<const llvm::MDNode>(typeMdNode->getOperand(3));
            assert(samplerMdNode);

            if (gla::CrackSamplerMd(samplerMdNode, mdSampler, mdType, mdSamplerDim, isArray, isShadow, mdBaseType))
               return GetSamplerType(mdSampler, mdBaseType, mdSamplerDim, isShadow, isArray);
         }

         glsl_base_type baseType = signedInt ? GLSL_TYPE_INT : GLSL_TYPE_UINT;

         if (gla::IsBoolean(type))
            baseType = GLSL_TYPE_BOOL;

         return_type = glsl_type::get_instance(baseType, 1, 1);
         break;
      }

   case llvm::Type::FloatTyID:
      return_type = glsl_type::get_instance(GLSL_TYPE_FLOAT, 1, 1);
      break;

   case llvm::Type::VoidTyID:
      return_type = glsl_type::get_instance(GLSL_TYPE_VOID, 1, 1);
      break;

   default:
      assert(0 && "unexpected LLVM type");
      break;
   }

   return typeMap[typeData] = return_type;
}


/**
 * -----------------------------------------------------------------------------
 * Translate function declaration to HIR
 * -----------------------------------------------------------------------------
 */
void MesaGlassTranslator::startFunctionDeclaration(const llvm::Type* type, llvm::StringRef name)
{
   // Initialize function state: we only ever translate one function at a time, as
   // they cannot be nested.
   resetFnTranslationState();
   
   // LunarG TODO: create parameter list, etc from
   // ir_function and ir_function_signature

   fnName       = ralloc_strdup(shader, name.str().c_str());
   fnReturnType = llvmTypeToHirType(type->getContainedType(0));

   // Create fn and signature objects
   fnFunction = new(shader) ir_function(fnName);
   fnSignature = new(shader) ir_function_signature(fnReturnType);
}


/**
 * -----------------------------------------------------------------------------
 * Finish function declaration
 * -----------------------------------------------------------------------------
 */
void MesaGlassTranslator::endFunctionDeclaration()
{
}


/**
 * -----------------------------------------------------------------------------
 * Start translation of function body
 * -----------------------------------------------------------------------------
 */
void MesaGlassTranslator::startFunctionBody()
{
   assert(state->current_function == NULL);

   // Start building new instruction list
   instructionStack.push_back(&fnSignature->body);

   if (!state->symbols->add_function(fnFunction)) {
      assert(0 && "LunarG TODO: Handle error");
      // Shouldn't fail, because semantic checking done by glslang
   }

   emit_function(state, fnFunction);
   fnFunction->add_signature(fnSignature);

   // Transfer the fn parameters to the HIR signature
   fnSignature->replace_parameters(&fnParameters);

   state->current_function = fnSignature; // LunarG TODO: might not need this
   state->symbols->push_scope();          // Or this?

   // For main, prepend prologue, if any
   while (!prologue.empty()) {
      addIRInstruction(prologue.front());
      prologue.pop_front();
   }
}


/**
 * -----------------------------------------------------------------------------
 * End translation of function body
 * -----------------------------------------------------------------------------
 */
void MesaGlassTranslator::endFunctionBody()
{
   fnSignature->is_defined = true; // tell linker function is defined

   assert(instructionStack.size() >= 2);  // at least 1 for shader, 1 for function
   instructionStack.pop_back();

   state->current_function = 0; // Reset things set in startFunctionBody
   state->symbols->pop_scope(); // ...

   // We've now handed off (to the HIR) ownership of these bits of translation
   // state, so don't free them!
   fnName = 0;
   fnReturnType = 0;
   fnSignature = 0;
   fnFunction = 0;
   fnParameters.make_empty();
}


/**
 * -----------------------------------------------------------------------------
 * Create IR constant value from Mesa constant
 * -----------------------------------------------------------------------------
 */
inline ir_rvalue* MesaGlassTranslator::newIRGlobal(const llvm::Value* value, const char* name)
{
   // LunarG TODO: maybe this can't be reached?  If we always see a Load first, that'll
   // take care of it.
   assert(0);
   return 0;
}


/**
 * -----------------------------------------------------------------------------
 * See if this constant is a zero
 * -----------------------------------------------------------------------------
 */
inline bool MesaGlassTranslator::isConstantZero(const llvm::Constant* constant) const
{
   // Code snippit borrowed from LunarGlass GLSL backend
   if (!constant || IsUndef(constant))
      return true;
   else if (llvm::isa<llvm::ConstantAggregateZero>(constant))
      return true;
   else
      return false;
}


/**
 * -----------------------------------------------------------------------------
 * Create simple scalar constant
 * -----------------------------------------------------------------------------
 */
template <typename T> inline T
MesaGlassTranslator::newIRScalarConstant(const llvm::Constant* constant) const
{
   if (isConstantZero(constant))
       return T(0);

   const llvm::Type* type = constant->getType();

   switch (type->getTypeID()) {
   case llvm::Type::IntegerTyID:
      {
         if (gla::IsBoolean(type)) {
            if (GetConstantInt(constant))
               return T(true);
            else
               return T(false);
         } else
            return T(GetConstantInt(constant));
      }

   case llvm::Type::FloatTyID:
      return T(GetConstantFloat(constant));

   default:
      assert(0 && "LunarG TODO: error in constant conversion"); // LunarG TODO: handle error
      return T(0);
   }
}


/**
 * -----------------------------------------------------------------------------
 * Create IR constant value from Mesa constant
 * LunarG TODO: !!!!PLACEHOLDER!!!!
 * -----------------------------------------------------------------------------
 */
inline ir_constant* MesaGlassTranslator::newIRConstant(const llvm::Value* value)
{
   const llvm::Constant* constant = llvm::dyn_cast<llvm::Constant>(value);
   const llvm::Type*     type     = constant->getType();

   assert(constant);

   // Handle undefined constant values
   if (llvm::isa<llvm::UndefValue>(constant))
      return addIRUndefined(type);

   const bool isZero = isConstantZero(constant);

   if (isZero)
      return ir_constant::zero(shader, llvmTypeToHirType(type));

   switch (type->getTypeID())
   {
   case llvm::Type::VectorTyID:
      {
         ir_constant_data data;
         const int count = gla::GetComponentCount(const_cast<llvm::Type*>(type));
         const llvm::ConstantDataSequential* seqData    = llvm::dyn_cast<const llvm::ConstantDataSequential>(constant);
         const llvm::ConstantVector*         vectorData = llvm::dyn_cast<const llvm::ConstantVector>(constant);

         const llvm::Constant* splatValue = 0;

         if (seqData)
            splatValue = seqData->getSplatValue();

         if (vectorData)
            splatValue = vectorData->getSplatValue();

         const glsl_type* glslType = 0;

         // LunarG TODO: must we handle other llvm Constant subclasses?
         for (int op=0; op<count; ++op) {
            const llvm::Constant* opData =
               isZero     ? 0 :
               splatValue ? splatValue :
               vectorData ? dyn_cast<const llvm::Constant>(vectorData->getOperand(op)) :
               seqData ? seqData->getElementAsConstant(op) :
               0;

            if (type->getContainedType(0) == type->getFloatTy(type->getContext())) {
               glslType = glsl_type::vec(count);
               data.f[op] = newIRScalarConstant<float>(opData);
            }
            else if (gla::IsBoolean(type->getContainedType(0))) {
               glslType = glsl_type::bvec(count);
               data.b[op] = newIRScalarConstant<bool>(opData);
            } else if (gla::IsInteger(type->getContainedType(0))) {
               // LunarG TODO: distinguish unsigned from signed
               glslType = glsl_type::ivec(count);
               data.i[op] = newIRScalarConstant<int>(opData);
            } else {
               // LunarG TODO: handle error
               assert(0 && "unexpected LLVM type");
            }
         }

         assert(glslType);

         return new(shader) ir_constant(glslType, &data);
      }
 
   case llvm::Type::IntegerTyID:
      // LunarG TODO: distinguish unsigned from signed, use getSExtValue or getZExtValue appropriately
      if (gla::IsBoolean(type))
         return new(shader) ir_constant(newIRScalarConstant<bool>(constant));
      else
         return new(shader) ir_constant(newIRScalarConstant<int>(constant));

   case llvm::Type::FloatTyID:
      return new(shader) ir_constant(newIRScalarConstant<float>(constant));

   case llvm::Type::ArrayTyID:
   case llvm::Type::StructTyID:
      {
         const llvm::ArrayType* arrayType     = llvm::dyn_cast<const llvm::ArrayType>(type);
         const int              arraySize     = arrayType->getNumElements();

         const llvm::ConstantDataSequential* dataSequential = llvm::dyn_cast<llvm::ConstantDataSequential>(constant);

         // Populate a list of entries
         exec_list constantValues;
         for (int element = 0; element < arraySize; ++element) {
            const llvm::Constant* constElement;
            if (dataSequential)
               constElement = dataSequential->getElementAsConstant(element);
            else
               constElement = llvm::dyn_cast<llvm::Constant>(constant->getOperand(element));

            constantValues.push_tail(newIRConstant(constElement));
         }

         return new(shader) ir_constant(llvmTypeToHirType(arrayType, 0, constant),
                                        &constantValues);
      }

   case llvm::Type::PointerTyID:
      assert(0 && "LunarG TODO: Handle pointer case");
      return 0;
   case llvm::Type::VoidTyID:
      assert(0 && "LunarG TODO: handle void case");
      return 0;
   default:
      assert(0 && "unexpected LLVM type");
      return new(shader) ir_constant(0.0f);
   }
}


/**
 * -----------------------------------------------------------------------------
 * Add type-safe undefined value in case someone looks up a not-defined value
 * -----------------------------------------------------------------------------
 */
inline ir_constant* MesaGlassTranslator::addIRUndefined(const llvm::Type* type)
{
   // LunarG TODO: add infolog warning about undefined values

   static const unsigned deadbeefU = 0xdeadbeef;
   static const float deadbeefF = *((float *)&deadbeefU);

   switch (type->getTypeID())
   {
   case llvm::Type::VectorTyID:
      {
         const int count = gla::GetComponentCount(const_cast<llvm::Type*>(type));

         if (type->getContainedType(0) == type->getFloatTy(type->getContext()))
            return new(shader) ir_constant(deadbeefF, count);
         else if (gla::IsBoolean(type->getContainedType(0)))
            return new(shader) ir_constant(false, count);
         else if (type->getContainedType(0) == type->getInt32Ty(type->getContext()))
            // LunarG TODO: distinguish unsigned from signed
            return new(shader) ir_constant(deadbeefU, count);
         else 
            assert(0 && "unexpected LLVM type");
         return new(shader) ir_constant(deadbeefF);
      }
 
   case llvm::Type::IntegerTyID:
      // LunarG TODO: distinguish unsigned from signed
      return new(shader) ir_constant(deadbeefU);

   case llvm::Type::FloatTyID:
      return new(shader) ir_constant(deadbeefF);

   case llvm::Type::ArrayTyID:
      {
         // LunarG TODO: this is likely not sufficient.  mats?  aofa?
         const llvm::ArrayType* arrayType = llvm::dyn_cast<const llvm::ArrayType>(type);
         assert(arrayType);

         const int         arraySize     = arrayType->getNumElements();
         const llvm::Type* containedType = arrayType->getContainedType(0);

         ralloc_array(shader, ir_constant*, arraySize);

         exec_list* constants = new(shader) exec_list;

         for (int e=0; e<arraySize; ++e) {
            ir_constant* elementValue = addIRUndefined(containedType)->as_constant();
            constants->push_tail(elementValue);
         }

         return new(shader) ir_constant(llvmTypeToHirType(type), constants);
      }

   case llvm::Type::StructTyID:
      assert(0 && "LunarG TODO: Handle struct case"); 

   case llvm::Type::PointerTyID: // fall through
   case llvm::Type::VoidTyID:    // fall through
   default:
      assert(0 && "unexpected LLVM type");
      return new(shader) ir_constant(deadbeefF);
   }
}


/**
 * -----------------------------------------------------------------------------
 * Get an IR value, either by looking up existing value, or making a
 * new constant or global
 * -----------------------------------------------------------------------------
 */
inline ir_rvalue*
MesaGlassTranslator::getIRValue(const llvm::Value* llvmValue, ir_instruction* instruction)
{
   tValueMap::const_iterator location = valueMap.find(llvmValue);

   // Insert into map if not there
   if (location == valueMap.end()) {
      // Handle constants
      if (llvm::isa<llvm::Constant>(llvmValue))
         instruction = newIRConstant(llvmValue);
      // Handle global variables
      else if (llvm::isa<llvm::GlobalVariable>(llvmValue))
         instruction = newIRGlobal(llvmValue);

      // If asking for something that doesn't exist, create default value
      if (instruction == 0)
         instruction = addIRUndefined(llvmValue->getType());

      location = valueMap.insert(tValueMap::value_type(llvmValue, instruction)).first;
   }

   // LunarG TODO: is there any reason to clone assign/call returns?

   ir_rvalue* rvalue;

   // Return appropriate value
   if (location->second->as_assignment()) {
      // For assignments, return a ref to the variable being assigned
      rvalue = location->second->as_assignment()->lhs;
   } else if (location->second->as_call()) {
      // for calls, return a ref to the return value
      rvalue = location->second->as_call()->return_deref;
   } else {
      // For all others, return the value, or a dereference
      if (location->second->as_rvalue()) {
         rvalue = location->second->as_rvalue();
      } else {
         assert(0 && "LunarG TODO: Handle error");
         return 0;
      }
   }

   return rvalue->clone(shader, 0);
}


/**
 * -----------------------------------------------------------------------------
 * Make up a name for a new variable
 * -----------------------------------------------------------------------------
 */
const char* MesaGlassTranslator::newName(const llvm::Value* llvmValue)
{
   static const char* baseLocalName = ".temp";

   // LunarG TODO: This string flotching is runtime inefficient.
   // Replace with something faster.
   if (!llvmValue->getName().empty())
      return ralloc_strdup(shader, (std::string(".") + llvmValue->getName().str()).c_str());

   return baseLocalName;
}


/**
 * -----------------------------------------------------------------------------
 * Add instruction (Raw): don't add map entry, just append to inst list
 * -----------------------------------------------------------------------------
 */
inline void
MesaGlassTranslator::addIRInstruction(ir_instruction* instruction, bool global)
{
   // Set variable as assigned to, if it is.
   if (instruction->as_assignment() &&
       instruction->as_assignment()->lhs->as_dereference_variable()) {
      instruction->as_assignment()->lhs->as_dereference_variable()->variable_referenced()->data.assigned = true;
   }

   if (global)
      instructionStack.front()->push_head(instruction);
   else
      instructionStack.back()->push_tail(instruction);
}


/**
 * -----------------------------------------------------------------------------
 * Add instruction (cooked): add to list on top of instruction list stack
 * -----------------------------------------------------------------------------
 */
inline void
MesaGlassTranslator::addIRInstruction(const llvm::Value* llvmValue, ir_instruction* instruction)
{
   assert(instruction);
   tValueMap::const_iterator location = valueMap.find(llvmValue);

   // Verify SSA-ness
   if (location != valueMap.end()) {
      assert(0 && "SSA failure!  Shouldn't add same value twice.");
   }

   const unsigned refCount = getRefCount(llvmValue);

   // These are instructions we must always insert directly
   const bool directInsertion =
      instruction->as_assignment() ||
      instruction->as_call() ||
      instruction->as_discard();

   // We never insert temp assignments for these
   const bool noInsertion =
      instruction->as_variable() ||
      instruction->as_dereference_variable();

   // Add it to the instruction list if needed.  Otherwise, don't; we'll look
   // it up later and use it in an expression tree
   if ((refCount > 1 || directInsertion) && !noInsertion) {
      // Create an assignment to a new local if needed
      if (!directInsertion) {
         ir_rvalue* rvalue = instruction->as_rvalue();

         if (!rvalue) {
            // Var<-var assignment
            assert(instruction->as_variable());
            rvalue = new(shader) ir_dereference_variable(instruction->as_variable());
         }

         ir_dereference* localVar = newIRVariableDeref(rvalue->type, newName(llvmValue), ir_var_auto);
         
         instruction = new(shader) ir_assignment(localVar, rvalue);
      }

      addIRInstruction(instruction);
   }

   valueMap[llvmValue] = instruction;
}


/**
 * -----------------------------------------------------------------------------
 * Add a builtin function call
 * -----------------------------------------------------------------------------
 */
inline void
MesaGlassTranslator::emitFn(const char* name, const llvm::Instruction* llvmInst)
{
   const llvm::CallInst* callInst = llvm::dyn_cast<llvm::CallInst>(llvmInst);

   const unsigned numArgs = callInst ? callInst->getNumArgOperands() : llvmInst->getNumOperands();

   // This is OK on the stack, because the ir_call constructor will move out of it.
   exec_list parameters;
   
   for (unsigned i=0; i < numArgs; ++i)
      parameters.push_tail(getIRValue(llvmInst->getOperand(i)));

   // Find the right function signature to call
   // This sets state->uses_builtin_functions
   ir_function_signature *sig =
      _mesa_glsl_find_builtin_function(state, name, &parameters);

   if (sig == 0) {
      assert(0 && "LunarG TODO: Handle error");
      return;
   }

   const std::string retName = std::string(name) + "_retval";
   ir_dereference* dest = newIRVariableDeref(llvmInst->getType(), llvmInst, retName, ir_var_auto);

   ir_call *call = new(shader) ir_call(sig, dest->as_dereference_variable(), &parameters);
   addIRInstruction(llvmInst, call);
}


/**
 * -----------------------------------------------------------------------------
 * Emit N-ary opcode
 * -----------------------------------------------------------------------------
 */
template <int ops>
inline void MesaGlassTranslator::emitOp(int irOp, const llvm::Instruction* llvmInst)
{
   static const int maxOp = 3;
   ir_rvalue* op[maxOp];
   assert(ops <= maxOp);

   const glsl_type *hirType = llvmTypeToHirType(llvmInst->getType(), 0, llvmInst);

   for (int i=0; i<maxOp; ++i)
      op[i] = ops > i ? getIRValue(llvmInst->getOperand(i)) : 0;

   ir_rvalue* result = new(shader) ir_expression(irOp, hirType, op[0], op[1], op[2]);
   assert(result && (op[0] || ops < 1) && (op[1] || ops < 2) && (op[2] || ops < 3));
   
   addIRInstruction(llvmInst, result);
}


/**
 * -----------------------------------------------------------------------------
 * Add alloc
 * -----------------------------------------------------------------------------
 */
inline void MesaGlassTranslator::emitIRalloca(const llvm::Instruction* llvmInst)
{
   llvm::Type* type = llvmInst->getType();
   if (const llvm::PointerType* pointer = llvm::dyn_cast<llvm::PointerType>(type))
      type = pointer->getContainedType(0);

   ir_rvalue* var = newIRVariableDeref(type, llvmInst, newName(llvmInst), ir_var_auto);

   addIRInstruction(llvmInst, var);
}


/**
 * -----------------------------------------------------------------------------
 * Add IR sign extension
 * -----------------------------------------------------------------------------
 */
inline void MesaGlassTranslator::emitIRSext(const llvm::Instruction* llvmInst)
{
   // LunarG TODO: handle arbitrary sign extension.  For now, just from 1 bit
   // ints.
   const glsl_type *hirType = llvmTypeToHirType(llvmInst->getType(), 0, llvmInst);
   ir_rvalue* op[3];

   const int count = gla::GetComponentCount(const_cast<llvm::Type*>(llvmInst->getType()));

   op[0] = getIRValue(llvmInst->getOperand(0));

   if (count == 1) {
      op[1] = new(shader) ir_constant(int(0xffffffff));
      op[2] = new(shader) ir_constant(int(0));
   } else {
      ir_constant_data data0, data1;
      data1.i[0] = data1.i[1] = data1.i[2] = data1.i[3] = int(0xffffffff);
      data0.i[0] = data0.i[1] = data0.i[2] = data0.i[3] = int(0x0);

      op[1] = new(shader) ir_constant(glsl_type::ivec(count), &data1);
      op[2] = new(shader) ir_constant(glsl_type::ivec(count), &data0);
   }

   // HIR wants the condition to be a vector of size of element, so we must
   // insert a broadcast swizzle if need be.
   if (op[0]->type->vector_elements != op[1]->type->vector_elements)
      op[0] = new(shader) ir_swizzle(op[0], 0, 0, 0, 0, op[1]->type->vector_elements);

   ir_rvalue* result = new(shader) ir_expression(ir_triop_csel, hirType, op[0], op[1], op[2]);
   assert(result && op[0] && op[1] && op[2]);
   
   addIRInstruction(llvmInst, result);
}


/**
 * -----------------------------------------------------------------------------
 * Emit vertex
 * -----------------------------------------------------------------------------
 */
inline void MesaGlassTranslator::emitIREmitVertex(const llvm::Instruction* llvmInst)
{
   addIRInstruction(new(shader) ir_emit_vertex());
}


/**
 * -----------------------------------------------------------------------------
 * End primitive
 * -----------------------------------------------------------------------------
 */
inline void MesaGlassTranslator::emitIREndPrimitive(const llvm::Instruction* llvmInst)
{
   addIRInstruction(new(shader) ir_end_primitive());
}


/**
 * -----------------------------------------------------------------------------
 * Emit a N-ary op of either logical or bitwise type, selecting from one of
 * two IR opcodes appropriately.
 * -----------------------------------------------------------------------------
 */
template <int ops>
inline void MesaGlassTranslator::emitOpBit(int irLogicalOp, int irBitwiseOp,
                                           const llvm::Instruction* llvmInst)
{
   const bool isBool = gla::IsBoolean(llvmInst->getOperand(0)->getType());

   return emitOp<ops>(isBool ? irLogicalOp : irBitwiseOp, llvmInst);
}


/**
 * -----------------------------------------------------------------------------
 * Emit comparison op
 * -----------------------------------------------------------------------------
 */
inline void MesaGlassTranslator::emitCmp(const llvm::Instruction* llvmInst)
{
   const llvm::CmpInst* cmp = llvm::dyn_cast<llvm::CmpInst>(llvmInst);
   
   // LunarG TODO: do we need to handle arrays / structs here?

   if (!cmp) {
      // LunarG TODO: handle internal error
      assert(0 && "LunarG TODO: Handle error");
   }

   int cmpOp;

   switch (cmp->getPredicate()) {
   case llvm::FCmpInst::FCMP_OEQ: // fall through
   case llvm::FCmpInst::FCMP_UEQ: // ...
   case llvm::ICmpInst::ICMP_EQ:  cmpOp = ir_binop_equal; break;

   case llvm::FCmpInst::FCMP_ONE: // fall through
   case llvm::FCmpInst::FCMP_UNE: // ...
   case llvm::ICmpInst::ICMP_NE:  cmpOp = ir_binop_nequal; break;

   case llvm::FCmpInst::FCMP_OGT: // fall through
   case llvm::FCmpInst::FCMP_UGT: // ...
   case llvm::ICmpInst::ICMP_UGT: // ...
   case llvm::ICmpInst::ICMP_SGT: cmpOp = ir_binop_greater; break;

   case llvm::FCmpInst::FCMP_OGE: // fall through...
   case llvm::FCmpInst::FCMP_UGE: // ...
   case llvm::ICmpInst::ICMP_UGE: // ...
   case llvm::ICmpInst::ICMP_SGE: cmpOp = ir_binop_gequal; break;

   case llvm::FCmpInst::FCMP_OLT: // fall through...
   case llvm::FCmpInst::FCMP_ULT: // ...
   case llvm::ICmpInst::ICMP_ULT: // ...
   case llvm::ICmpInst::ICMP_SLT: cmpOp = ir_binop_less; break;

   case llvm::FCmpInst::FCMP_OLE: // fall through...
   case llvm::FCmpInst::FCMP_ULE: // ...
   case llvm::ICmpInst::ICMP_ULE: // ...
   case llvm::ICmpInst::ICMP_SLE: cmpOp = ir_binop_lequal; break;
      
   default:
      assert(0 && "LunarG TODO: Handle error");
      cmpOp = ir_binop_equal;
   }

   // LunarG TODO: handle arrays, structs
   return emitOp<2>(cmpOp, llvmInst);
}


/**
 * -----------------------------------------------------------------------------
 * Add a conditional discard
 * NOTE: CURRENTLY DISABLED pending backend support.  See:
 *   brw_fs_visitor.cpp:fs_visitor::visit(ir_discard *ir)
 * Comment "FINISHME" there.  When that's done, this can be enabled
 * by removing our override of hoistDiscards
 * -----------------------------------------------------------------------------
 */
inline void MesaGlassTranslator::emitIRDiscardCond(const llvm::CallInst* llvmInst)
{
   ir_rvalue *op0      = getIRValue(llvmInst->getOperand(0));
   ir_discard* discard = new(shader) ir_discard(op0);

   addIRInstruction(llvmInst, discard);
}


/**
 * -----------------------------------------------------------------------------
 * Add a conditional discard
 * NOTE: CURRENTLY DISABLED pending backend support.  See:
 *   brw_fs_visitor.cpp:fs_visitor::visit(ir_discard *ir)
 * Comment "FINISHME" there.  When that's done, this can be enabled
 * by removing our override of hoistDiscards
 * -----------------------------------------------------------------------------
 */
inline void MesaGlassTranslator::emitIRFTransform(const llvm::CallInst* llvmInst)
{
   exec_list parameters;

   ir_function_signature *sig =
      _mesa_glsl_find_builtin_function(state, "ftransform", &parameters);

   // LunarG TODO: don't assume ir_var_temporary here
   ir_dereference* dest = newIRVariableDeref(llvmInst->getType(), llvmInst,
                                             newName(llvmInst), ir_var_temporary);

   // LunarG TODO: Should we insert a prototype?
   ir_call *call = new(shader) ir_call(sig, dest->as_dereference_variable(), &parameters);

   addIRInstruction(llvmInst, call);
}

/**
 * -----------------------------------------------------------------------------
 * Declare phi output
 * -----------------------------------------------------------------------------
 */
void MesaGlassTranslator::declarePhiCopy(const llvm::Value* dst)
{
   valueMap[dst] = newIRVariableDeref(dst->getType(), dst, newName(dst), ir_var_auto);
}


/**
 * -----------------------------------------------------------------------------
 * Emit phi function copies
 * -----------------------------------------------------------------------------
 */
void MesaGlassTranslator::addPhiCopy(const llvm::Value* dst, const llvm::Value* src)
{
   ir_rvalue* irDst = getIRValue(dst);
   ir_rvalue* irSrc = getIRValue(src);

   // LunarG TODO: temporary hack to avoid samplers participating in phis.
   if (irSrc->type->is_sampler()) {
      valueMap[dst] = irSrc;
      return;
   }

   ir_assignment* assign  = new(shader) ir_assignment(irDst, irSrc);
   addIRInstruction(assign);
}


/**
 * -----------------------------------------------------------------------------
 * Emit IR if statement from ir_rvalue
 * -----------------------------------------------------------------------------
 */
inline void MesaGlassTranslator::addIf(ir_rvalue* cond, bool invert)
{
   if (invert)
      cond = new(shader) ir_expression(ir_unop_logic_not, cond->type, cond);

   ir_if *const ifStmt = new(shader) ir_if(cond);

   ifStack.push_back(ifStmt);
   instructionStack.push_back(&ifStmt->then_instructions);
   state->symbols->push_scope();
}


/**
 * -----------------------------------------------------------------------------
 * Emit "if" (but not else clause)
 * -----------------------------------------------------------------------------
 */
void MesaGlassTranslator::addIf(const llvm::Value* cond, bool invert)
{
   return addIf(getIRValue(cond), invert);
}


/**
 * -----------------------------------------------------------------------------
 * Emit else clause
 * -----------------------------------------------------------------------------
 */
void MesaGlassTranslator::addElse()
{
   state->symbols->pop_scope();
   instructionStack.pop_back();
   instructionStack.push_back(&ifStack.back()->else_instructions);
   state->symbols->push_scope();
}


/**
 * -----------------------------------------------------------------------------
 * Emit endif
 * -----------------------------------------------------------------------------
 */
void MesaGlassTranslator::addEndif()
{
   assert(!ifStack.empty());

   state->symbols->pop_scope();
   instructionStack.pop_back();

   addIRInstruction(ifStack.back());

   ifStack.pop_back();
}


/**
 * -----------------------------------------------------------------------------
 * Emit conditional loop
 * -----------------------------------------------------------------------------
 */
void MesaGlassTranslator::beginConditionalLoop()
{
   assert(0 && "LunarG TODO: ...");
}


/**
 * -----------------------------------------------------------------------------
 * Emit conditional loop
 * -----------------------------------------------------------------------------
 */
void MesaGlassTranslator::beginSimpleConditionalLoop(const llvm::CmpInst* cmp,
                                                     const llvm::Value* op0,
                                                     const llvm::Value* op1,
                                                     bool invert)
{
  // Start loop body
   beginLoop();

   // LunarG TODO: Why don't we see both of these through normal channels?
   // If we haven't already processed the extract elements, do that now.
   if (!valueEntryExists(op0))
      addInstruction(dyn_cast<const llvm::Instruction>(op0), false);
   
   if (!valueEntryExists(op1))
      addInstruction(dyn_cast<const llvm::Instruction>(op1), false);
   
   emitCmp(cmp);  // emit loop comparison
   addLoopExit(cmp, invert);
}


/**
 * -----------------------------------------------------------------------------
 * Emit simple inductive loop (for i=0; i<const; ++i) ...
 * -----------------------------------------------------------------------------
 */
void MesaGlassTranslator::beginSimpleInductiveLoop(const llvm::PHINode* phi, unsigned count)
{
   // newIRVariableDeref(glsl_type::uint_type, ".loop.idx", ir_var_auto);
   ir_rvalue* loopVar = getIRValue(phi);

   // we don't have to initialize loopVar to 0: the BottomIR will produce that for us

   // Start loop body
   beginLoop();

   // Add a conditional break
   ir_rvalue* cmpLt = new(shader) ir_expression(ir_binop_gequal, glsl_type::bool_type,
                                                loopVar, new(shader) ir_constant(int(count)));

   addIRLoopExit(cmpLt);

   // Create terminator statement (for ++index)
   ir_assignment* terminator =
      new(shader) ir_assignment(loopVar->clone(shader, 0),
                                new(shader) ir_expression(ir_binop_add, loopVar->type,
                                                          loopVar->clone(shader, 0), 
                                                          new(shader) ir_constant(1)));

   loopTerminatorStack.back() = terminator;
}


/**
 * -----------------------------------------------------------------------------
 * Emit inductive loop
 * -----------------------------------------------------------------------------
 */
void MesaGlassTranslator::beginSimpleInductiveLoop(const llvm::PHINode* phi, const llvm::Value* count)
{
   assert(0 && "LunarG TODO: ...");
}


/**
 * -----------------------------------------------------------------------------
 * Emit begin loop
 * -----------------------------------------------------------------------------
 */
void MesaGlassTranslator::beginLoop()
{
   ir_loop *const loopStmt = new(shader) ir_loop();

   loopStack.push_back(loopStmt);
   instructionStack.push_back(&loopStmt->body_instructions);
   state->symbols->push_scope();

   loopTerminatorStack.push_back(0);
}


/**
 * -----------------------------------------------------------------------------
 * Emit end loop
 * -----------------------------------------------------------------------------
 */
void MesaGlassTranslator::endLoop()
{
   // Handle any loop terminator instruction
   if (loopTerminatorStack.back())
      addIRInstruction(loopTerminatorStack.back());

   loopTerminatorStack.pop_back();

   state->symbols->pop_scope();
   instructionStack.pop_back();

   addIRInstruction(loopStack.back());

   loopStack.pop_back();
}


/**
 * -----------------------------------------------------------------------------
 * Add IR loop exit statement from ir_rvalue
 * -----------------------------------------------------------------------------
 */
inline void MesaGlassTranslator::addIRLoopExit(ir_rvalue* condition, bool invert)
{
   if (condition)
      addIf(condition, invert);

   ir_loop_jump *const loopExit = new(shader) ir_loop_jump(ir_loop_jump::jump_break);
   addIRInstruction(loopExit);

   if (condition)
      addEndif();
}


/**
 * -----------------------------------------------------------------------------
 * Add loop exit
 * -----------------------------------------------------------------------------
 */
void MesaGlassTranslator::addLoopExit(const llvm::Value* condition, bool invert)
{
   addIRLoopExit(condition ? getIRValue(condition) : 0, invert);
}


/**
 * -----------------------------------------------------------------------------
 * Add loop continue
 * -----------------------------------------------------------------------------
 */
void MesaGlassTranslator::addLoopBack(const llvm::Value* condition, bool invert)
{
   ir_loop_jump *const loopContinue = new(shader) ir_loop_jump(ir_loop_jump::jump_continue);
   addIRInstruction(loopContinue);
}


/**
 * -----------------------------------------------------------------------------
 * Translate call
 * -----------------------------------------------------------------------------
 */
inline void MesaGlassTranslator::emitIRCall(const llvm::CallInst* llvmInst)
{
   assert(0 && "LunarG TODO: handle call");
}


/**
 * -----------------------------------------------------------------------------
 * Translate return
 * -----------------------------------------------------------------------------
 */
inline void MesaGlassTranslator::emitIRReturn(const llvm::Instruction* llvmInst, bool lastBlock)
{
   if (llvmInst->getNumOperands() > 0)
      addIRInstruction(new(shader) ir_return(getIRValue(llvmInst->getOperand(0))));
   else if (!lastBlock) // don't add return in last shader block.
      addIRInstruction(new(shader) ir_return);
}


/**
 * -----------------------------------------------------------------------------
 * Track maximum array element used
 * If index < 0, means we use the entire array (probably indirect indexing)
 * -----------------------------------------------------------------------------
 */
inline void MesaGlassTranslator::trackMaxArrayElement(ir_rvalue* rvalue, int index) const
{
   if (ir_dereference_variable *deref = rvalue->as_dereference_variable()) {
      if (ir_variable* var = deref->variable_referenced()) {
         if (index >= 0)
            var->data.max_array_access = std::max(var->data.max_array_access, unsigned(index));
         else
            var->data.max_array_access = deref->type->length - 1;
      }
   }
}


/**
 * -----------------------------------------------------------------------------
 * Deference IO intrinsic down to a single slot
 * Borrorwed & modified from LunarGlass GLSL Bottom translator DereferenceName
 * -----------------------------------------------------------------------------
 */
ir_rvalue*
MesaGlassTranslator::dereferenceIO(ir_rvalue* aggregate,
                                   const llvm::Type* type,
                                   const llvm::MDNode* mdAggregate,
                                   int slotOffset,
                                   EMdTypeLayout& mdTypeLayout)
{
   if (type->getTypeID() == llvm::Type::PointerTyID) {
      type = type->getContainedType(0);

      aggregate = dereferenceIO(aggregate, type, mdAggregate, slotOffset, mdTypeLayout);
   } else if (type->getTypeID() == llvm::Type::StructTyID) {
      int field = 0;
      int operand;
      const llvm::StructType* structType = llvm::dyn_cast<const llvm::StructType>(type);
      const llvm::Type* fieldType;
      do {
         operand = GetAggregateMdSubAggregateOp(field);
         if (operand >= int(mdAggregate->getNumOperands())) {
            assert(operand < int(mdAggregate->getNumOperands()));
            return aggregate;
         }
         fieldType = structType->getContainedType(field);
         const int fieldSize = CountSlots(fieldType);
         if (fieldSize > slotOffset)
            break;
         slotOffset -= fieldSize;
         ++field;
      } while (true);

      const char *field_name = ralloc_strdup(shader,
                                             mdAggregate->getOperand(GetAggregateMdNameOp(field))->getName().str().c_str());

      const llvm::MDNode* subMdAggregate = llvm::dyn_cast<const llvm::MDNode>(mdAggregate->getOperand(operand));

      aggregate = new(shader) ir_dereference_record(aggregate, field_name);
      aggregate = dereferenceIO(aggregate, fieldType, subMdAggregate, slotOffset, mdTypeLayout);

   } else if (type->getTypeID() == llvm::Type::ArrayTyID) {
      const llvm::ArrayType* arrayType = llvm::dyn_cast<const llvm::ArrayType>(type);
      const int elementSize = CountSlots(arrayType->getContainedType(0));
      const int element = slotOffset / elementSize;
      slotOffset = slotOffset % elementSize;

      ir_rvalue* indexVal = new(shader) ir_constant(element);

      aggregate = new(shader) ir_dereference_array(aggregate, indexVal);
      aggregate = dereferenceIO(aggregate, arrayType->getContainedType(0), mdAggregate, slotOffset, mdTypeLayout);

      trackMaxArrayElement(aggregate, element);
  } else if (mdAggregate)
      mdTypeLayout = GetMdTypeLayout(mdAggregate);

   return aggregate;
}


/**
 * -----------------------------------------------------------------------------
 * Translate writedata intrinsics
 * -----------------------------------------------------------------------------
 */
inline void
MesaGlassTranslator::emitIRIOIntrinsic(const llvm::IntrinsicInst* llvmInst, bool input)
{
   std::string         name;
   llvm::Type*         mdType;
   EMdInputOutput      mdQual;
   EMdPrecision        mdPrecision;
   EMdTypeLayout       mdLayout;
   int                 layoutLocation;
   const llvm::MDNode* mdAggregate;
   const llvm::MDNode* dummySampler;
   int                 interpMode;

   const llvm::MDNode* mdNode = llvmInst->getMetadata(input ? gla::InputMdName : gla::OutputMdName);
   assert(mdNode);

   // Glean information from metadata for intrinsic
   gla::CrackIOMd(mdNode, name, mdQual, mdType, mdLayout, mdPrecision, layoutLocation, dummySampler, mdAggregate, interpMode);
   
   const glsl_type* irType  = llvmTypeToHirType(mdType->getContainedType(0), mdNode, llvmInst);

   const int slotOffset  = GetConstantInt(llvmInst->getOperand(0)) - layoutLocation;
   const ir_variable_mode irMode = input ? ir_var_shader_in : ir_var_shader_out;

   ir_rvalue* ioVarDeref = newIRVariableDeref(irType, name, irMode);
   
   ir_variable* ioVar    = ioVarDeref->as_dereference_variable()->variable_referenced();

   // ioVar->data.how_declared  = ...
   ioVar->data.used          = true;
   // ioVar->data.location      = layoutLocation;
   ioVar->data.index         = slotOffset;
   //   ioVar->data.explicit_index = ???
   // ioVar->data.explicit_location = false;
   //   ioVar->data.explicit_binding
   //   ioVar->data.has_initializer
   //   ioVar->data.binding
   //   ioVar->data.invariant
   // ioVar->data.interpolation = InterpolationQualifierToIR(gla::EInterpolationMethod(interpMode));

   ioVarDeref = dereferenceIO(ioVarDeref, mdType, mdAggregate, slotOffset, mdLayout);

   // LunarG TODO: handle write mask, add to ir_variable's mask
   if (input) {
      addIRInstruction(llvmInst, ioVarDeref);
   } else {
      ir_assignment* assign = new(shader) ir_assignment(ioVarDeref, getIRValue(llvmInst->getOperand(2)));
      addIRInstruction(llvmInst, assign);
   }
}


/**
 * -----------------------------------------------------------------------------
 * Translate swizzle intrinsics
 *   (swiz components ...)
 * -----------------------------------------------------------------------------
 */
inline void MesaGlassTranslator::emitIRSwizzle(const llvm::IntrinsicInst* llvmInst)
{
   const llvm::Value*    src  = llvmInst->getOperand(0);
   const llvm::Constant* mask = llvm::dyn_cast<llvm::Constant>(llvmInst->getOperand(1));
   assert(mask);

   // LunarG TODO: Is this sufficient for all types of swizzles?  E.g, broadcasting swizzles?

   unsigned components[4] = { 0, 0, 0, 0 };
   unsigned componentCount = gla::GetComponentCount(llvmInst);

   for (unsigned i = 0; i < componentCount; ++i)
      if (IsDefined(mask->getAggregateElement(i)))
         components[i] = gla::GetConstantInt(mask->getAggregateElement(i));

   ir_swizzle* swizzle = new(shader) ir_swizzle(getIRValue(src), components, componentCount);

   addIRInstruction(llvmInst, swizzle);
}


/**
 * -----------------------------------------------------------------------------
 * Translate insertion intrinsics
 *   turns into N instances of:
 *   (assign (x) (var_ref var) (expression...))
 * Handles vector or scalar sources
 * -----------------------------------------------------------------------------
 */
inline void MesaGlassTranslator::emitIRMultiInsert(const llvm::IntrinsicInst* llvmInst)
{
   const int wmask = gla::GetConstantInt(llvmInst->getOperand(1));

   // LunarG TODO: optimize identity case? investigate general optimization
   // in this space.  For example, if all components are overwritten from
   // the same source, we can just pass on a swizzle and avoid the
   // temp the the assignment.

   ir_dereference* localTmp = newIRVariableDeref(llvmInst->getType(), llvmInst,
                                                 newName(llvmInst), ir_var_auto);

   ir_variable* var = localTmp->as_dereference_variable()->variable_referenced();

   const bool writingAllComponents =
      var->type->is_vector() &&
      ((1<<var->type->components())-1) == wmask;

   // Copy original value to new temporary
   // LunarG TODO: is it better to just assign the not-written components?
   if (!writingAllComponents) {
      ir_assignment* assign = new(shader) ir_assignment(localTmp->clone(shader, 0),
                                                        getIRValue(llvmInst->getOperand(0)));
      addIRInstruction(assign);
   }
      
   unsigned numComponents        = 0;
   unsigned components[4]        = { 0, 0, 0, 0 };
   int      partialMask          = 0;
   const llvm::Value *prevSource = 0;

   // LunarG TODO: this may not be general enough.  E.g, array operands?
   for (int i = 0; i < 4; ++i) {
      const llvm::Value* srcOperand = llvmInst->getOperand((i+1) * 2);
      const llvm::Constant* swizOffset = llvm::dyn_cast<llvm::Constant>(llvmInst->getOperand(i*2 + 3));

      // If source changed, do the ones we have so far
      if (!IsSameSource(srcOperand, prevSource) && numComponents > 0) {
         ir_swizzle*    swizzle = new(shader) ir_swizzle(getIRValue(prevSource), components, numComponents);
         ir_assignment* assign  = new(shader) ir_assignment(localTmp->clone(shader, 0), swizzle, 0, partialMask);
         addIRInstruction(assign);
         numComponents = 0;
         partialMask   = 0;
      }

      if (IsDefined(swizOffset)) {
         prevSource = srcOperand;
         components[numComponents++] = gla::GetConstantInt(swizOffset);
         partialMask |= 1<<i;
      }
   }

   // Do any remainder
   if (numComponents > 0) {
      ir_swizzle*    swizzle = new(shader) ir_swizzle(getIRValue(prevSource), components, numComponents);
      ir_assignment* assign  = new(shader) ir_assignment(localTmp->clone(shader, 0), swizzle, 0, partialMask);
      addIRInstruction(assign);
   }

   // LunarG TODO: maybe if it's all one source and we're overwriting the whole thing, it's just a
   // swizzle, and we don't have to make a temp assignment.

   // Add the local temp we created for this multi-insert
   addIRInstruction(llvmInst, localTmp);
}


/**
 * -----------------------------------------------------------------------------
 * Emit LLVM saturate intrinsic
 * -----------------------------------------------------------------------------
 */
inline void MesaGlassTranslator::emitIRSaturate(const llvm::CallInst* llvmInst)
{
   const glsl_type *hirType = llvmTypeToHirType(llvmInst->getType(), 0, llvmInst);

   ir_rvalue* minOp = new(shader) ir_expression(ir_binop_min, hirType,
                                                getIRValue(llvmInst->getOperand(0)),
                                                new(shader) ir_constant(1.0f));

   ir_rvalue* result = new(shader) ir_expression(ir_binop_max, hirType,
                                                 minOp, new(shader) ir_constant(0.0f));
                                           
   addIRInstruction(llvmInst, result);
}


/**
 * -----------------------------------------------------------------------------
 * Emit LLVM clamp intrinsic.  We prefer LunarGlass to decompose this for us,
 * but can leave this here for testing purposes.
 * -----------------------------------------------------------------------------
 */
inline void MesaGlassTranslator::emitIRClamp(const llvm::CallInst* llvmInst)
{
   const glsl_type *hirType = llvmTypeToHirType(llvmInst->getType(), 0, llvmInst);

   ir_rvalue* minOp = new(shader) ir_expression(ir_binop_max, hirType,
                                                getIRValue(llvmInst->getOperand(0)),
                                                getIRValue(llvmInst->getOperand(1)));

   ir_rvalue* result = new(shader) ir_expression(ir_binop_min, hirType,
                                                 minOp,
                                                 getIRValue(llvmInst->getOperand(2)));

   addIRInstruction(llvmInst, result);
}


/**
 * -----------------------------------------------------------------------------
 * Determine glsl matrix type
 * -----------------------------------------------------------------------------
 */
inline const glsl_type* MesaGlassTranslator::glslMatType(int numCols, int numRows) const
{
   // If all columns are from the same source, we can use mat*vec or vec*mat
   // Determine the glsl_type to use:
   if (numRows == numCols) {
      // Square matrix
      switch (numRows) {
      case 2: return glsl_type::mat2_type;
      case 3: return glsl_type::mat3_type; 
      case 4: return glsl_type::mat4_type; 
      default: assert(0); return 0;  // LunarG TODO: handle error...
      }
   } else {
      // non-square matrix
      if (numCols == 2 && numRows == 3)
         return glsl_type::mat2x3_type;
      else if (numCols == 2 && numRows == 4)
         return glsl_type::mat2x4_type;
      else if (numCols == 3 && numRows == 2)
         return glsl_type::mat3x2_type;
      else if (numCols == 3 && numRows == 4)
         return glsl_type::mat3x4_type;
      else if (numCols == 4 && numRows == 2)
         return glsl_type::mat4x2_type;
      else if (numCols == 4 && numRows == 3)
         return glsl_type::mat4x3_type;
      else {
         assert(0); return 0; // LunarG TODO: handle error...
      }
   }
}


/**
 * -----------------------------------------------------------------------------
 * Load, potentially from GEP chain offset
 * typeOverride lets us overrule the internal type, for example, if a higher
 * level mat*vec intrinsics knows the type should be a matrix.
 * -----------------------------------------------------------------------------
 */
inline ir_rvalue*
MesaGlassTranslator::makeIRLoad(const llvm::Instruction* llvmInst, const glsl_type* typeOverride)
{
   const llvm::GetElementPtrInst* gepInst = getGepAsInst(llvmInst->getOperand(0));
   const llvm::MDNode*            mdNode  = llvmInst->getMetadata(UniformMdName);

   llvm::StringRef name;

   // If this load doesn't have a metadata node, try to find one we've created
   // during variable declarations.  We use rendezvous-by-name to that end.
   // We might not find one though, e.g, if this load is not from a global.

   // LunarG TODO: ...

   if (gepInst)
      name = gepInst->getOperand(0)->getName();
   else 
      name = llvmInst->getOperand(0)->getName();

   if (!mdNode)
      mdNode = typenameMdMap[name];

   // Look up the ir_variable_mode we remembered during the global declaration
   const ir_variable_mode irMode = ir_variable_mode(globalVarModeMap[name]);

   if (mdNode)
      name = mdNode->getOperand(0)->getName();

   // Handle types for both GEP chain loads and non-GEPs.
   const llvm::Type* srcType =
      (gepInst ? gepInst->getPointerOperand() : llvmInst->getOperand(0))->getType();

   if (srcType->getTypeID() == llvm::Type::PointerTyID)
      srcType = srcType->getContainedType(0);

   const glsl_type* irType = llvmTypeToHirType(srcType, mdNode, llvmInst);

   ir_rvalue* load;

   // Handle GEP traversal
   if (gepInst) {
      const llvm::Value* gepSrc = gepInst->getPointerOperand();
      ir_rvalue* aggregate;

      // LunarG TODO: For globals, do we really have to look this up from the
      // global decl?  The global decl can't put any llvm::Value in the map,
      // because it doesn't have a llvm::Value, so we can't use the normal
      // scheme. This seems ugly: probably just overlooking the "right" way.
      // Needs work...
      if (globalDeclMap.find(name) != globalDeclMap.end())
         aggregate = newIRVariableDeref(irType, name, irMode);
      else
         aggregate = getIRValue(gepSrc);
   
      load = traverseGEP(gepInst, aggregate, 0);
   } else {
      // LunarG TODO: shouldn't be making a new variable each time
      load = newIRVariableDeref(irType, name, irMode);
   }

   // Handle type overriding
   if (typeOverride)
      load->type = typeOverride;

   return load;
}


/**
 * -----------------------------------------------------------------------------
 * Create an IR matrix object by direct read from a uniform matrix,
 * or moves of column vectors into a temp matrix.
 * -----------------------------------------------------------------------------
 */
inline ir_rvalue*
MesaGlassTranslator::intrinsicMat(const llvm::Instruction* llvmInst,
                                  int firstColumn, int numCols, int numRows)
{
   const llvm::Value* matSource = 0;

   // If the source is from a uniform, we'll make a new uniform matrix.
   // Otherwise, we'll form a matrix by copying the columns into a new mat,
   // and hope downstream copy propagation optimizes them away.
   for (int col=0; col<numCols; ++col) {
      const int colPos = col + firstColumn;

      if (const llvm::ExtractValueInst* extractValue =
          llvm::dyn_cast<const llvm::ExtractValueInst>(llvmInst->getOperand(colPos))) {

         if (col == 0) {
            matSource = extractValue->getOperand(0);
         } else {
            if (matSource != extractValue->getOperand(0))
               matSource = 0;
         }
      }
   }

   const glsl_type* matType    = glslMatType(numCols, numRows);
   assert(matType);

   ir_rvalue* matrix = 0;

   if (matSource) {
      const llvm::Instruction* matSrcInst = dyn_cast<const llvm::Instruction>(matSource);

      const llvm::GetElementPtrInst* gepInst = getGepAsInst(matSrcInst->getOperand(0));
      llvm::StringRef name;

      if (gepInst)
         name = gepInst->getOperand(0)->getName();
      else 
         name = llvmInst->getOperand(0)->getName();

      const ir_variable_mode irMode = ir_variable_mode(globalVarModeMap[name]);

      // Single mat source may have come from a uniform load, or a previous mat*mat
      if (matSrcInst->getOpcode() == llvm::Instruction::Load &&
          irMode == ir_var_uniform) {
         matrix = makeIRLoad(dyn_cast<const llvm::Instruction>(matSource), matType);
      } else if (matSrcInst->getOpcode() == llvm::Instruction::Call) {
         const llvm::IntrinsicInst* intrinsic = dyn_cast<const llvm::IntrinsicInst>(matSrcInst);

         if (intrinsic) {
            switch (intrinsic->getIntrinsicID()) {
            case llvm::Intrinsic::gla_fMatrix2TimesMatrix2:
            case llvm::Intrinsic::gla_fMatrix2TimesMatrix3:
            case llvm::Intrinsic::gla_fMatrix2TimesMatrix4:
            case llvm::Intrinsic::gla_fMatrix3TimesMatrix2:
            case llvm::Intrinsic::gla_fMatrix3TimesMatrix3:
            case llvm::Intrinsic::gla_fMatrix3TimesMatrix4:
            case llvm::Intrinsic::gla_fMatrix4TimesMatrix2:
            case llvm::Intrinsic::gla_fMatrix4TimesMatrix3:
            case llvm::Intrinsic::gla_fMatrix4TimesMatrix4:
               matrix = getIRValue(matSource);
               break;
            default:
               break;
               // we're going to load it the hard way
            }
         }
      }
   }

   if (!matrix) {
      // If we can't use mat*vec directly, issue separate moves into a temp matrix,
      // and multiply from that.
      matrix = newIRVariableDeref(matType, ralloc_strdup(shader, ".mat.temp"), ir_var_auto);

      for (int col=0; col < numCols; ++col) {
         const int colPos = col + firstColumn;

         ir_rvalue* indexVal = new(shader) ir_constant(col);
         ir_rvalue* column   = new(shader) ir_dereference_array(matrix->clone(shader, 0), indexVal);

         addIRInstruction(new(shader) ir_assignment(column, getIRValue(llvmInst->getOperand(colPos))));
      }
   }

   return matrix;
}



/**
 * -----------------------------------------------------------------------------
 * mat*mat intrinsics
 * -----------------------------------------------------------------------------
 */
inline void
MesaGlassTranslator::emitIRMatTimesMat(const llvm::Instruction* llvmInst,
                                       int numLeftCols, int numRightCols)
{
   // Size of column vector
   const int leftRows  =
      gla::GetComponentCount(const_cast<llvm::Type*>(llvmInst->getOperand(0)->getType()));
   const int rightRows =
      gla::GetComponentCount(const_cast<llvm::Type*>(llvmInst->getOperand(numLeftCols)->getType()));

   // LLVM produces a struct result, which isn't what we want to be making.  So, we'll
   // override that with the type we know it ought to be.
   const glsl_type* resultType = glslMatType(numLeftCols, rightRows);

   ir_rvalue* leftMat  = intrinsicMat(llvmInst, 0, numLeftCols, leftRows);
   ir_rvalue* rightMat = intrinsicMat(llvmInst, numLeftCols, numRightCols, rightRows);
   
   ir_rvalue* result   = new(shader) ir_expression(ir_binop_mul, resultType, leftMat, rightMat);

   addIRInstruction(llvmInst, result);
}


/**
 * -----------------------------------------------------------------------------
 * mat*Vec and vec*mat intrinsics
 * -----------------------------------------------------------------------------
 */
inline void
MesaGlassTranslator::emitIRMatMul(const llvm::Instruction* llvmInst, int numCols, bool matLeft)
{
   const int firstColumn = matLeft ? 0 : 1;
   const int vecPos      = matLeft ? numCols : 0;

   const llvm::Value* vecSource = llvmInst->getOperand(vecPos);

   // Size of column vector
   const int vecSize = 
      gla::GetComponentCount(const_cast<llvm::Type*>(llvmInst->getOperand(vecPos)->getType()));

   ir_rvalue* result = 0;

   const glsl_type* resultType = llvmTypeToHirType(llvmInst->getType(), 0, llvmInst);

   ir_rvalue* vector = getIRValue(vecSource);
   ir_rvalue* matrix = intrinsicMat(llvmInst, firstColumn, numCols, vecSize);

   // Issue the matrix multiply, in the requested order
   if (matLeft)
      result = new(shader) ir_expression(ir_binop_mul, resultType, matrix, vector);
   else
      result = new(shader) ir_expression(ir_binop_mul, resultType, vector, matrix);

   addIRInstruction(llvmInst, result);
}


/**
 * -----------------------------------------------------------------------------
 * Translate intrinsic
 * -----------------------------------------------------------------------------
 */
inline void MesaGlassTranslator::emitIRIntrinsic(const llvm::IntrinsicInst* llvmInst)
{
   switch (llvmInst->getIntrinsicID()) {
   // Handle WriteData ------------------------------------------------------------------------
   case llvm::Intrinsic::gla_writeData:                         // fall through
   case llvm::Intrinsic::gla_fWriteData:                        return emitIRIOIntrinsic(llvmInst, false);

   // Handle ReadData -------------------------------------------------------------------------
   case llvm::Intrinsic::gla_readData:                          // fall through
   case llvm::Intrinsic::gla_fReadData:                         // ...
   case llvm::Intrinsic::gla_fReadInterpolant:                  return emitIRIOIntrinsic(llvmInst, true);

   case llvm::Intrinsic::invariant_end:                         assert(0); break; // LunarG TODO:
   case llvm::Intrinsic::invariant_start:                       assert(0); break; // LunarG TODO:
   case llvm::Intrinsic::lifetime_end:                          assert(0); break; // LunarG TODO:
   case llvm::Intrinsic::lifetime_start:                        assert(0); break; // LunarG TODO:

   // Handle Texturing ------------------------------------------------------------------------
   case llvm::Intrinsic::gla_queryTextureSize:                  // ... fall through...
   case llvm::Intrinsic::gla_queryTextureSizeNoLod:             return emitFn("textureSize", llvmInst);
   case llvm::Intrinsic::gla_fQueryTextureLod:                  return emitFn("textureLod", llvmInst);
   // LunarG TODO: Goo: 430 Functionality: textureQueryLevels()
   // case llvm::Intrinsic::gla_queryTextureLevels: // LunarG TODO:

   case llvm::Intrinsic::gla_textureSample:                     // fall through...
   case llvm::Intrinsic::gla_fTextureSample:                    // ...
   case llvm::Intrinsic::gla_rTextureSample1:                   // ...
   case llvm::Intrinsic::gla_fRTextureSample1:                  // ...
   case llvm::Intrinsic::gla_rTextureSample2:                   // ...
   case llvm::Intrinsic::gla_fRTextureSample2:                  // ...
   case llvm::Intrinsic::gla_rTextureSample3:                   // ...
   case llvm::Intrinsic::gla_fRTextureSample3:                  // ...
   case llvm::Intrinsic::gla_rTextureSample4:                   // ...
   case llvm::Intrinsic::gla_fRTextureSample4:                  // ...
   case llvm::Intrinsic::gla_textureSampleLodRefZ:              // ...
   case llvm::Intrinsic::gla_fTextureSampleLodRefZ:             // ...
   case llvm::Intrinsic::gla_rTextureSampleLodRefZ1:            // ...
   case llvm::Intrinsic::gla_fRTextureSampleLodRefZ1:           // ...
   case llvm::Intrinsic::gla_rTextureSampleLodRefZ2:            // ...
   case llvm::Intrinsic::gla_fRTextureSampleLodRefZ2:           // ...
   case llvm::Intrinsic::gla_rTextureSampleLodRefZ3:            // ...
   case llvm::Intrinsic::gla_fRTextureSampleLodRefZ3:           // ...
   case llvm::Intrinsic::gla_rTextureSampleLodRefZ4:            // ...
   case llvm::Intrinsic::gla_fRTextureSampleLodRefZ4:           // ...
   case llvm::Intrinsic::gla_textureSampleLodRefZOffset:        // ...
   case llvm::Intrinsic::gla_fTextureSampleLodRefZOffset:       // ...
   case llvm::Intrinsic::gla_rTextureSampleLodRefZOffset1:      // ...
   case llvm::Intrinsic::gla_fRTextureSampleLodRefZOffset1:     // ...
   case llvm::Intrinsic::gla_rTextureSampleLodRefZOffset2:      // ...
   case llvm::Intrinsic::gla_fRTextureSampleLodRefZOffset2:     // ...
   case llvm::Intrinsic::gla_rTextureSampleLodRefZOffset3:      // ...
   case llvm::Intrinsic::gla_fRTextureSampleLodRefZOffset3:     // ...
   case llvm::Intrinsic::gla_rTextureSampleLodRefZOffset4:      // ...
   case llvm::Intrinsic::gla_fRTextureSampleLodRefZOffset4:     // ...
   case llvm::Intrinsic::gla_textureSampleLodRefZOffsetGrad:    // ...
   case llvm::Intrinsic::gla_fTextureSampleLodRefZOffsetGrad:   // ...
   case llvm::Intrinsic::gla_rTextureSampleLodRefZOffsetGrad1:  // ...
   case llvm::Intrinsic::gla_fRTextureSampleLodRefZOffsetGrad1: // ...
   case llvm::Intrinsic::gla_rTextureSampleLodRefZOffsetGrad2:  // ...
   case llvm::Intrinsic::gla_fRTextureSampleLodRefZOffsetGrad2: // ...
   case llvm::Intrinsic::gla_rTextureSampleLodRefZOffsetGrad3:  // ...
   case llvm::Intrinsic::gla_fRTextureSampleLodRefZOffsetGrad3: // ...
   case llvm::Intrinsic::gla_rTextureSampleLodRefZOffsetGrad4:  // ...
   case llvm::Intrinsic::gla_fRTextureSampleLodRefZOffsetGrad4: // ...
   case llvm::Intrinsic::gla_texelFetchOffset:                  // ... 
   case llvm::Intrinsic::gla_fTexelFetchOffset:                 return emitIRTexture(llvmInst, false);
   // Gather
   case llvm::Intrinsic::gla_texelGather:                       // Fall through ...
   case llvm::Intrinsic::gla_fTexelGather:                      // ...
   case llvm::Intrinsic::gla_texelGatherOffset:                 // ...
   case llvm::Intrinsic::gla_fTexelGatherOffset:                // ...
   case llvm::Intrinsic::gla_texelGatherOffsets:                // ...
   case llvm::Intrinsic::gla_fTexelGatherOffsets:               return emitIRTexture(llvmInst, true);

   // Handle MultInsert -----------------------------------------------------------------------
   case llvm::Intrinsic::gla_fMultiInsert:                      // fall through...
   case llvm::Intrinsic::gla_multiInsert:                       return emitIRMultiInsert(llvmInst);

   // Handle Swizzles -------------------------------------------------------------------------
   case llvm::Intrinsic::gla_swizzle:                           // fall through...
   case llvm::Intrinsic::gla_fSwizzle:                          return emitIRSwizzle(llvmInst);

   // Handle FP and integer intrinsics --------------------------------------------------------
   case llvm::Intrinsic::gla_abs:                               // fall through...
   case llvm::Intrinsic::gla_fAbs:                              return emitOp<1>(ir_unop_abs, llvmInst);

   case llvm::Intrinsic::gla_sMin:                              // fall through...
   case llvm::Intrinsic::gla_uMin:                              // ...
   case llvm::Intrinsic::gla_fMin:                              return emitOp<2>(ir_binop_min, llvmInst);

   case llvm::Intrinsic::gla_sMax:                              // fall through...
   case llvm::Intrinsic::gla_uMax:                              // ...
   case llvm::Intrinsic::gla_fMax:                              return emitOp<2>(ir_binop_max, llvmInst);

   case llvm::Intrinsic::gla_sClamp:                            // fall through...
   case llvm::Intrinsic::gla_uClamp:                            // ...
   case llvm::Intrinsic::gla_fClamp:                            return emitIRClamp(llvmInst);

   case llvm::Intrinsic::gla_fRadians:                          return emitFn("radians", llvmInst);
   case llvm::Intrinsic::gla_fDegrees:                          return emitFn("degrees", llvmInst);
   case llvm::Intrinsic::gla_fSin:                              return emitOp<1>(ir_unop_sin, llvmInst);
   case llvm::Intrinsic::gla_fCos:                              return emitOp<1>(ir_unop_cos, llvmInst);
   case llvm::Intrinsic::gla_fTan:                              return emitFn("tan", llvmInst);
   case llvm::Intrinsic::gla_fAsin:                             return emitFn("asin", llvmInst);
   case llvm::Intrinsic::gla_fAcos:                             return emitFn("acos", llvmInst);
   case llvm::Intrinsic::gla_fAtan:                             // fall through...
   case llvm::Intrinsic::gla_fAtan2:                            return emitFn("atan", llvmInst);
   case llvm::Intrinsic::gla_fSinh:                             return emitFn("sinh", llvmInst);
   case llvm::Intrinsic::gla_fCosh:                             return emitFn("cosh", llvmInst);
   case llvm::Intrinsic::gla_fTanh:                             return emitFn("tanh", llvmInst);
   case llvm::Intrinsic::gla_fAsinh:                            return emitFn("asinh", llvmInst);
   case llvm::Intrinsic::gla_fAcosh:                            return emitFn("acosh", llvmInst);
   case llvm::Intrinsic::gla_fAtanh:                            return emitFn("atanh", llvmInst);
   case llvm::Intrinsic::gla_fPow:                              return emitOp<2>(ir_binop_pow,  llvmInst);
      //case llvm::Intrinsic::gla_fPowi:                        assert(0); break; // LunarG TODO:
   case llvm::Intrinsic::gla_fExp:                              return emitOp<1>(ir_unop_exp,   llvmInst);
   case llvm::Intrinsic::gla_fLog:                              return emitOp<1>(ir_unop_log,   llvmInst);
   case llvm::Intrinsic::gla_fExp2:                             return emitOp<1>(ir_unop_exp2,  llvmInst);
   case llvm::Intrinsic::gla_fLog2:                             return emitOp<1>(ir_unop_log2,  llvmInst);
   case llvm::Intrinsic::gla_fExp10:                            assert(0); break; return; // Let LunarGlass decompose
   case llvm::Intrinsic::gla_fLog10:                            assert(0); break; return; // Let LunarGlass decompose
   case llvm::Intrinsic::gla_fSqrt:                             return emitOp<1>(ir_unop_sqrt,  llvmInst);
   case llvm::Intrinsic::gla_fInverseSqrt:                      return emitOp<1>(ir_unop_rsq,   llvmInst);
   case llvm::Intrinsic::gla_fSign:                             // fall through...
   case llvm::Intrinsic::gla_sign:                              return emitOp<1>(ir_unop_sign,  llvmInst);
   case llvm::Intrinsic::gla_fFloor:                            return emitOp<1>(ir_unop_floor, llvmInst);
   case llvm::Intrinsic::gla_fCeiling:                          return emitOp<1>(ir_unop_ceil,  llvmInst);
   case llvm::Intrinsic::gla_fRoundEven:                        return emitOp<1>(ir_unop_round_even, llvmInst);
   case llvm::Intrinsic::gla_fRoundZero:                        return emitOp<1>(ir_unop_trunc,  llvmInst);
   case llvm::Intrinsic::gla_fRoundFast:                        assert(0); break; // LunarG TODO:
   case llvm::Intrinsic::gla_fFraction:                         return emitOp<1>(ir_unop_fract, llvmInst);
   case llvm::Intrinsic::gla_fModF:                             return emitFn("modf", llvmInst);
   case llvm::Intrinsic::gla_fMix:                              // Fall through...
   case llvm::Intrinsic::gla_fbMix:                             return emitOp<3>(ir_triop_lrp,  llvmInst);
   case llvm::Intrinsic::gla_fStep:                             return emitFn("step", llvmInst);
   case llvm::Intrinsic::gla_fSmoothStep:                       return emitFn("smoothstep", llvmInst);
   case llvm::Intrinsic::gla_fIsNan:                            return emitFn("isnan", llvmInst);
   case llvm::Intrinsic::gla_fIsInf:                            return emitFn("isinf", llvmInst);
   case llvm::Intrinsic::gla_fSaturate:                         return emitIRSaturate(llvmInst);
   // LunarG TODO: Presently LunarGlass doesn't fuse mul/add itself.  It may help
   // if some optimization would do so.
   case llvm::Intrinsic::gla_fFma:                              return emitOp<3>(ir_triop_fma,  llvmInst);

   // Handle integer-only ops -----------------------------------------------------------------
   case llvm::Intrinsic::gla_addCarry:                          return emitOp<2>(ir_binop_carry, llvmInst);
   case llvm::Intrinsic::gla_subBorrow:                         return emitOp<2>(ir_binop_borrow, llvmInst);
   case llvm::Intrinsic::gla_umulExtended:                      return emitFn("umulExtended", llvmInst);
   case llvm::Intrinsic::gla_smulExtended:                      return emitFn("smulExtended", llvmInst);

   // Handle mat*vec & vec*mat intrinsics -----------------------------------------------------
   case llvm::Intrinsic::gla_fMatrix2TimesVector:               return emitIRMatMul(llvmInst, 2, true);
   case llvm::Intrinsic::gla_fMatrix3TimesVector:               return emitIRMatMul(llvmInst, 3, true);
   case llvm::Intrinsic::gla_fMatrix4TimesVector:               return emitIRMatMul(llvmInst, 4, true);
   case llvm::Intrinsic::gla_fVectorTimesMatrix2:               return emitIRMatMul(llvmInst, 2, false);
   case llvm::Intrinsic::gla_fVectorTimesMatrix3:               return emitIRMatMul(llvmInst, 3, false);
   case llvm::Intrinsic::gla_fVectorTimesMatrix4:               return emitIRMatMul(llvmInst, 4, false);

   // Handle mat*mat -- -----------------------------------------------------------------------
   case llvm::Intrinsic::gla_fMatrix2TimesMatrix2:              return emitIRMatTimesMat(llvmInst, 2, 2);
   case llvm::Intrinsic::gla_fMatrix2TimesMatrix3:              return emitIRMatTimesMat(llvmInst, 2, 3);
   case llvm::Intrinsic::gla_fMatrix2TimesMatrix4:              return emitIRMatTimesMat(llvmInst, 2, 4);
   case llvm::Intrinsic::gla_fMatrix3TimesMatrix2:              return emitIRMatTimesMat(llvmInst, 3, 2);
   case llvm::Intrinsic::gla_fMatrix3TimesMatrix3:              return emitIRMatTimesMat(llvmInst, 3, 3);
   case llvm::Intrinsic::gla_fMatrix3TimesMatrix4:              return emitIRMatTimesMat(llvmInst, 3, 4);
   case llvm::Intrinsic::gla_fMatrix4TimesMatrix2:              return emitIRMatTimesMat(llvmInst, 4, 2);
   case llvm::Intrinsic::gla_fMatrix4TimesMatrix3:              return emitIRMatTimesMat(llvmInst, 4, 3);
   case llvm::Intrinsic::gla_fMatrix4TimesMatrix4:              return emitIRMatTimesMat(llvmInst, 4, 4);

   // Handle bit operations -------------------------------------------------------------------
   case llvm::Intrinsic::gla_fFloatBitsToInt:                   return emitFn("floatBitsToInt", llvmInst);
   case llvm::Intrinsic::gla_fIntBitsTofloat:                   return emitFn("intBitsToFloat", llvmInst);
   case llvm::Intrinsic::gla_sBitFieldExtract:                  // Fall through...
   case llvm::Intrinsic::gla_uBitFieldExtract:                  return emitOp<3>(ir_triop_bitfield_extract, llvmInst);
   case llvm::Intrinsic::gla_bitFieldInsert:                    return emitOp<3>(ir_triop_bfi, llvmInst); // LunarG TODO: verify
   case llvm::Intrinsic::gla_bitReverse:                        return emitOp<1>(ir_unop_bitfield_reverse, llvmInst);
   case llvm::Intrinsic::gla_bitCount:                          return emitOp<1>(ir_unop_bit_count, llvmInst);
   case llvm::Intrinsic::gla_findLSB:                           return emitOp<1>(ir_unop_find_lsb, llvmInst);
   case llvm::Intrinsic::gla_sFindMSB:                          // Fall through...
   case llvm::Intrinsic::gla_uFindMSB:                          return emitOp<1>(ir_unop_find_msb, llvmInst);

   // Handle pack/unpack ----------------------------------------------------------------------
   case llvm::Intrinsic::gla_fFrexp:                            return emitFn("frexp", llvmInst);
   case llvm::Intrinsic::gla_fLdexp:                            return emitFn("ldexp", llvmInst);
   case llvm::Intrinsic::gla_fPackUnorm2x16:                    return emitOp<1>(ir_unop_pack_unorm_2x16, llvmInst);
   case llvm::Intrinsic::gla_fUnpackUnorm2x16:                  return emitOp<1>(ir_unop_unpack_unorm_2x16, llvmInst);

   case llvm::Intrinsic::gla_fPackSnorm2x16:                    return emitOp<1>(ir_unop_pack_snorm_2x16, llvmInst);
   case llvm::Intrinsic::gla_fUnpackSnorm2x16:                  return emitOp<1>(ir_unop_unpack_snorm_2x16, llvmInst);

   case llvm::Intrinsic::gla_fPackHalf2x16:                     return emitOp<1>(ir_unop_pack_half_2x16, llvmInst);
   case llvm::Intrinsic::gla_fUnpackHalf2x16:                   return emitOp<1>(ir_unop_unpack_half_2x16, llvmInst);

   case llvm::Intrinsic::gla_fPackUnorm4x8:                     return emitOp<1>(ir_unop_pack_unorm_4x8, llvmInst);
   case llvm::Intrinsic::gla_fPackSnorm4x8:                     return emitOp<1>(ir_unop_pack_snorm_4x8, llvmInst);

   case llvm::Intrinsic::gla_fUnpackUnorm4x8:                   return emitOp<1>(ir_unop_unpack_unorm_4x8, llvmInst);
   case llvm::Intrinsic::gla_fUnpackSnorm4x8:                   return emitOp<1>(ir_unop_unpack_snorm_4x8, llvmInst);

   case llvm::Intrinsic::gla_fPackDouble2x32:                   assert(0); break; // LunarG TODO:
   case llvm::Intrinsic::gla_fUnpackDouble2x32:                 assert(0); break; // LunarG TODO:

   // Handle geometry ops ---------------------------------------------------------------------
   case llvm::Intrinsic::gla_fLength:                           return emitFn("length", llvmInst);
   case llvm::Intrinsic::gla_fDistance:                         return emitFn("distance", llvmInst);
   case llvm::Intrinsic::gla_fDot2:                             // fall through...
   case llvm::Intrinsic::gla_fDot3:                             // ...
   case llvm::Intrinsic::gla_fDot4:                             return emitOp<2>(ir_binop_dot, llvmInst);
   case llvm::Intrinsic::gla_fCross:                            return emitFn("cross", llvmInst);
   case llvm::Intrinsic::gla_fNormalize:                        return emitFn("normalize", llvmInst);
   case llvm::Intrinsic::gla_fNormalize3D:                      assert(0); break; // LunarG TODO:
   case llvm::Intrinsic::gla_fLit:                              assert(0); break; // LunarG TODO:
   case llvm::Intrinsic::gla_fFaceForward:                      return emitFn("faceforward", llvmInst);
   case llvm::Intrinsic::gla_fReflect:                          return emitFn("reflect", llvmInst);
   case llvm::Intrinsic::gla_fRefract:                          return emitFn("refract", llvmInst);

   // Handle derivative and transform----------------------------------------------------------
   case llvm::Intrinsic::gla_fDFdx:                             return emitOp<1>(ir_unop_dFdx, llvmInst);
   case llvm::Intrinsic::gla_fDFdy:                             return emitOp<1>(ir_unop_dFdy, llvmInst);
   case llvm::Intrinsic::gla_fFilterWidth:                      return emitFn("fwidth", llvmInst);

   // Handle vector logical ops ---------------------------------------------------------------
   case llvm::Intrinsic::gla_not:                               return emitFn("not", llvmInst);
   case llvm::Intrinsic::gla_any:                               return emitOp<1>(ir_unop_any, llvmInst);
   case llvm::Intrinsic::gla_all:                               return emitFn("all", llvmInst);

   case llvm::Intrinsic::gla_discardConditional:                return emitIRDiscardCond(llvmInst);

   // Handle fixed transform ------------------------------------------------------------------
   case llvm::Intrinsic::gla_fFixedTransform:                   return emitIRFTransform(llvmInst);

   // Control ---------------------------------------------------------------------------------
   case llvm::Intrinsic::gla_barrier:                           assert(0); break; // LunarG TODO:
   case llvm::Intrinsic::gla_memoryBarrier:                     assert(0); break; // LunarG TODO:
   case llvm::Intrinsic::gla_memoryBarrierAtomicCounter:        assert(0); break; // LunarG TODO:
   case llvm::Intrinsic::gla_memoryBarrierBuffer:               assert(0); break; // LunarG TODO:
   case llvm::Intrinsic::gla_memoryBarrierImage:                assert(0); break; // LunarG TODO:
   case llvm::Intrinsic::gla_memoryBarrierShared:               assert(0); break; // LunarG TODO:
   case llvm::Intrinsic::gla_groupMemoryBarrier:                assert(0); break; // LunarG TODO:

   // Geometry --------------------------------------------------------------------------------
   case llvm::Intrinsic::gla_emitVertex:                        return emitIREmitVertex(llvmInst);
   case llvm::Intrinsic::gla_endPrimitive:                      return emitIREndPrimitive(llvmInst);
   case llvm::Intrinsic::gla_emitStreamVertex:                  assert(0); break; // LunarG TODO:
   case llvm::Intrinsic::gla_endStreamPrimitive:                assert(0); break; // LunarG TODO:

   default:
      assert(0 && "LunarG TODO: Handle unknown intrinsic");
      break;
   }
}


/**
 * -----------------------------------------------------------------------------
 * Translate call or intrinsic
 * -----------------------------------------------------------------------------
 */
inline void MesaGlassTranslator::emitIRCallOrIntrinsic(const llvm::Instruction* llvmInst)
{

   if (const llvm::IntrinsicInst* intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(llvmInst)) {
      emitIRIntrinsic(intrinsic);
   } else {
      const llvm::CallInst* call = llvm::dyn_cast<llvm::CallInst>(llvmInst);
      assert(call);
      emitIRCall(call);
   }
}


/**
 * -----------------------------------------------------------------------------
 * Vector component extraction
 *   <result> = extractelement <n x <ty>> <val>, i32 <idx>    ; yields type <ty>
 *   index can be constant or variable
 * -----------------------------------------------------------------------------
 */
inline void MesaGlassTranslator::emitIRExtractElement(const llvm::Instruction* llvmInst)
{
   ir_rvalue *vector   = getIRValue(llvmInst->getOperand(0));
   ir_rvalue *index    = getIRValue(llvmInst->getOperand(1));
   assert(vector && index);

   if (index->as_constant()) {
      unsigned component = index->as_constant()->get_uint_component(0);
      const unsigned components[4] = { component, component, component, component };
      ir_swizzle* swizzle = new(shader) ir_swizzle(vector, components, 1);
      addIRInstruction(llvmInst, swizzle);
   } else {
      emitOp<2>(ir_binop_vector_extract, llvmInst);
   }
}


/**
 * -----------------------------------------------------------------------------
 * Emit IR select (ternary op)
 * -----------------------------------------------------------------------------
 */
inline void MesaGlassTranslator::emitIRSelect(const llvm::Instruction* llvmInst)
{
   const glsl_type *hirType = llvmTypeToHirType(llvmInst->getType(), 0, llvmInst);
   ir_rvalue* op[3];

   op[0] = getIRValue(llvmInst->getOperand(0));
   op[1] = getIRValue(llvmInst->getOperand(1));
   op[2] = getIRValue(llvmInst->getOperand(2));

   ir_rvalue* result;

   // HIR can't switch arrays, etc, from a single boolean.  Create
   // if/else/endif in that case.
   if (op[1]->type->is_array() || op[1]->type->is_matrix() ||op[1]->type->is_record()) {
      result = newIRVariableDeref(hirType, newName(llvmInst), ir_var_auto);

      addIf(op[0]); {
         addIRInstruction(new(shader) ir_assignment(result, op[1]));
      } addElse(); {
         addIRInstruction(new(shader) ir_assignment(result->clone(shader, 0), op[2]));
      } addEndif();
   } else {
      // HIR wants the condition to be a vector of size of element, so we must
      // insert a broadcast swizzle if need be.
      if (op[1]->type->is_vector() && !op[0]->type->is_vector())
         op[0] = new(shader) ir_swizzle(op[0], 0, 0, 0, 0, op[1]->type->vector_elements);

      result = new(shader) ir_expression(ir_triop_csel, hirType, op[0], op[1], op[2]);
   }
   
   assert(result && op[0] && op[1] && op[2]);

   addIRInstruction(llvmInst, result);
}


/**
 * -----------------------------------------------------------------------------
 * Fix IR Lvalues
 * When seeing a GEP chain, we don't yet know whether it will be for an
 * LValue or RValue reference.  Mostly the HIR is the same for both, so
 * this works out, but for vector component references, we must produce
 * different HIR.  This change adds support for doing that by converting
 * the R-value form to the L-value form, so that we don't need to guess
 * about it earlier in time than we see the use.  (Alternately,  we could
 * analyze the situation in a pre-pass).
 * -----------------------------------------------------------------------------
 */
ir_instruction* MesaGlassTranslator::fixIRLValue(ir_rvalue* lhs, ir_rvalue* rhs)
{
   // convert:
   //  (assign (vector_extract vec comp) elementVal)
   // to;
   //  (assign vec (vector_insert vec elementval comp))
   // LunarG TODO: Sometimes we might not need the assign

   if (ir_expression* lval = lhs->as_expression()) {
      if (lval->operation == ir_binop_vector_extract) {
         ir_expression* vecIns =
            new(shader) ir_expression(ir_triop_vector_insert,
                                      lval->operands[0]->type,
                                      lval->operands[0], 
                                      rhs,
                                      lval->operands[1]);

         return new(shader) ir_assignment(lval->operands[0]->clone(shader, 0),
                                          vecIns);
      }
   }

   // Otherwise, return normal assignment
   return new(shader) ir_assignment(lhs, rhs);
}


/**
 * -----------------------------------------------------------------------------
 * Load, potentially from GEP chain offset
 * -----------------------------------------------------------------------------
 */
inline void MesaGlassTranslator::emitIRLoad(const llvm::Instruction* llvmInst)
{
   addIRInstruction(llvmInst, makeIRLoad(llvmInst));
}


/**
 * -----------------------------------------------------------------------------
 * Store, potentially to GEP chain offset
 * -----------------------------------------------------------------------------
 */
inline void MesaGlassTranslator::emitIRStore(const llvm::Instruction* llvmInst)
{
   const llvm::Value*             src     = llvmInst->getOperand(0);
   const llvm::Value*             dst     = llvmInst->getOperand(1);
   const llvm::GetElementPtrInst* gepInst = getGepAsInst(dst);

   assert(llvm::isa<llvm::PointerType>(dst->getType()));

   ir_rvalue* irDst;

   if (gepInst) {
      const llvm::MDNode* mdNode;
      const llvm::Type*   mdType;
      const llvm::Type*   mdAggregateType;

      FindGepType(gepInst, mdType, mdAggregateType, mdNode);

      const glsl_type* irType = llvmTypeToHirType(mdType, mdNode, llvmInst);

      const llvm::Value* gepSrc = gepInst->getPointerOperand();

      // If this is the first write to this aggregate, make up a new one.
      const tValueMap::const_iterator location = valueMap.find(gepSrc);
      if (location == valueMap.end()) {
         llvm::StringRef name = dst->getName();

         if (gepSrc)
            name = gepInst->getPointerOperand()->getName();

         const ir_variable_mode irMode = ir_variable_mode(globalVarModeMap[name]);

         irDst = newIRVariableDeref(irType, name, irMode);

         valueMap.insert(tValueMap::value_type(gepSrc, irDst)).first;
      }

      irDst = traverseGEP(gepInst, getIRValue(gepSrc), 0);
   } else {
      const glsl_type* irType = llvmTypeToHirType(dst->getType()->getContainedType(0), 0, llvmInst);

      llvm::StringRef name = dst->getName();
      const ir_variable_mode irMode = ir_variable_mode(globalVarModeMap[name]);

      irDst = newIRVariableDeref(irType, name, irMode);
   }

   addIRInstruction(llvmInst, fixIRLValue(irDst, getIRValue(src)));
}


/**
 * -----------------------------------------------------------------------------
 * Vector component insertion
 * -----------------------------------------------------------------------------
 */
inline void MesaGlassTranslator::emitIRInsertElement(const llvm::Instruction* llvmInst)
{
   // ir_tripop_vector_insert
   assert(0 && "LunarG TODO: handle insertElement");
}


/**
 * -----------------------------------------------------------------------------
 *  Traverse one step of a dereference chain
 * -----------------------------------------------------------------------------
 */
ir_rvalue* MesaGlassTranslator::dereferenceGep(const llvm::Type*& type, ir_rvalue* aggregate,
                                               llvm::Value* operand, int index,
                                               const llvm::MDNode*& mdAggregate,
                                               EMdTypeLayout* mdTypeLayout)
{
   if (operand) {
      if (llvm::isa<const llvm::ConstantInt>(operand))
         index = GetConstantInt(operand);
      else
         index = -1;
   }

   switch (type->getTypeID()) {
   case llvm::Type::StructTyID:
   case llvm::Type::ArrayTyID:
   case llvm::Type::VectorTyID:
      if (aggregate->type->is_array() || aggregate->type->is_matrix()) {
         ir_rvalue* indexVal;

         if (index < 0) { // Indirect indexing
            indexVal = getIRValue(operand);
            trackMaxArrayElement(aggregate, -1);
         } else { // Direct indexing
            indexVal = new(shader) ir_constant(index);
            trackMaxArrayElement(aggregate, index);
         }

         type = type->getContainedType(0);

         return new(shader) ir_dereference_array(aggregate, indexVal);
      } else if (aggregate->type->is_vector()) {
         type = type->getContainedType(0);

         // if (index < 0) { // indirec indexing
            return new(shader) ir_expression(ir_binop_vector_extract,
                                             llvmTypeToHirType(type), aggregate,
                                             getIRValue(operand));
         // } else { // direct indexing
         //    const unsigned components[4] = { unsigned(index), 0, 0, 0 };
         //    return new(shader) ir_swizzle(aggregate, components, 1);
         // }
      } else if (aggregate->type->is_record()) {
         assert(index >= 0); // LunarG TODO: handle error: struct deference only supports const index

         type = type->getContainedType(index);

         // Deference the metadata aggregate 
         if (mdAggregate) {
            const int aggOp = GetAggregateMdSubAggregateOp(index);
            if (int(mdAggregate->getNumOperands()) <= aggOp) {
               assert(0 && "LunarG TODO: Handle error");
               mdAggregate = 0;
            } else {
               mdAggregate = llvm::dyn_cast<llvm::MDNode>(mdAggregate->getOperand(aggOp));
            }

            if (mdAggregate && mdTypeLayout)
               *mdTypeLayout = GetMdTypeLayout(mdAggregate);
         }

         const char *field_name = ralloc_strdup(shader, aggregate->type->fields.structure[index].name);
         return new(shader) ir_dereference_record(aggregate, field_name);
      }
      // fall through to error case if it wasn't an array or a struct

   default:
      // LunarG TODO: handle unexpected type
      assert(0 && "LunarG TODO: Handle unexpected type error");
      return 0;
   }
}


/**
 * -----------------------------------------------------------------------------
 * Traverse GEP instruction chain
 * -----------------------------------------------------------------------------
 */

inline void MesaGlassTranslator::FindGepType(const llvm::Instruction* llvmInst,
                                             const llvm::Type*& type,
                                             const llvm::Type*& aggregateType,
                                             const llvm::MDNode*& mdNode)
{
   aggregateType = type = llvmInst->getOperand(0)->getType();

   while (type->getTypeID() == llvm::Type::PointerTyID)
      type = type->getContainedType(0);

   while (aggregateType->getTypeID() == llvm::Type::PointerTyID || aggregateType->getTypeID() == llvm::Type::ArrayTyID)
      aggregateType = aggregateType->getContainedType(0);
   
   mdNode = typeMdAggregateMap[aggregateType];
}


/**
 * -----------------------------------------------------------------------------
 * Traverse GEP instruction chain
 * -----------------------------------------------------------------------------
 */
inline ir_rvalue* MesaGlassTranslator::traverseGEP(const llvm::Instruction* llvmInst,
                                                   ir_rvalue* aggregate,
                                                   EMdTypeLayout* mdTypeLayout)
{
   // *** Function borrowed from LunarGlass GLSL backend, modified slightly ***
   const llvm::MDNode* mdAggregate;
   const llvm::Type*   mdType;
   const llvm::Type*   mdAggregateType;

   FindGepType(llvmInst, mdType, mdAggregateType, mdAggregate);

   // Register type in the type map:
   const llvm::StructType* structType = llvm::dyn_cast<const llvm::StructType>(mdAggregateType);
   if (structType) {
      const llvm::StringRef structName = structType->isLiteral() ? "" : structType->getName();
      typenameMdMap[structName] = mdAggregate;
   }

   if (const llvm::GetElementPtrInst* gepInst = getGepAsInst(llvmInst)) {
      // Start at operand 2 since indices 0 and 1 give you the base and are handled before traverseGep
      const llvm::Type* gepType = gepInst->getPointerOperandType()->getContainedType(0);
      for (unsigned int op = 2; op < gepInst->getNumOperands(); ++op)
         aggregate = dereferenceGep(gepType, aggregate, gepInst->getOperand(op), -1, mdAggregate, mdTypeLayout);

   } else if (const llvm::InsertValueInst* insertValueInst = llvm::dyn_cast<const llvm::InsertValueInst>(llvmInst)) {
      const llvm::Type* gepType = insertValueInst->getAggregateOperand()->getType();            
      for (llvm::InsertValueInst::idx_iterator iter = insertValueInst->idx_begin(), end = insertValueInst->idx_end();
           iter != end; ++iter)
         aggregate = dereferenceGep(gepType, aggregate, 0, *iter, mdAggregate);

   } else if (const llvm::ExtractValueInst* extractValueInst = llvm::dyn_cast<const llvm::ExtractValueInst>(llvmInst)) {
      const llvm::Type* gepType = extractValueInst->getAggregateOperand()->getType();  
      for (llvm::ExtractValueInst::idx_iterator iter = extractValueInst->idx_begin(), end = extractValueInst->idx_end();
           iter != end; ++iter)
         aggregate = dereferenceGep(gepType, aggregate, 0, *iter, mdAggregate);

   } else {
      assert(0 && "non-GEP in traverseGEP");
      return 0;
   }

   return aggregate;
}


/**
 * -----------------------------------------------------------------------------
 * Array component or struct extraction
 *   <result> = extractvalue <aggregate type> <val>, <idx>{, <idx>}*
 *      (array_ref (var_ref ...) (constant int (0) ))
 * -----------------------------------------------------------------------------
 */
inline void MesaGlassTranslator::emitIRExtractValue(const llvm::Instruction* llvmInst)
{
   const llvm::ExtractValueInst* extractValueInst = llvm::dyn_cast<const llvm::ExtractValueInst>(llvmInst);
   assert(llvmInst);

   const llvm::Value* aggregate = extractValueInst->getAggregateOperand();
   assert(aggregate);

   addIRInstruction(llvmInst, traverseGEP(extractValueInst, getIRValue(aggregate), 0));
}


/**
 * -----------------------------------------------------------------------------
 * Array component insertion
 * -----------------------------------------------------------------------------
 */
inline void MesaGlassTranslator::emitIRInsertValue(const llvm::Instruction* llvmInst)
{
   const llvm::InsertValueInst* insertValueInst = llvm::dyn_cast<const llvm::InsertValueInst>(llvmInst);

   ir_rvalue* aggregate  = getIRValue(insertValueInst->getAggregateOperand());
   ir_rvalue* gep        = traverseGEP(insertValueInst, aggregate, 0);
   ir_rvalue* src        = getIRValue(insertValueInst->getInsertedValueOperand());
   ir_assignment* assign = new(shader) ir_assignment(gep, src);

   addIRInstruction(assign);
   addIRInstruction(llvmInst, aggregate);
}


/**
 * -----------------------------------------------------------------------------
 * Translate instruction
 * -----------------------------------------------------------------------------
 */
void MesaGlassTranslator::addInstruction(const llvm::Instruction* llvmInst, bool lastBlock, bool referencedOutsideScope)
{
    if (referencedOutsideScope) {
       // emitGlaValueDeclaration(llvmInst, referencedOutsideScope);
    }

    switch (llvmInst->getOpcode()) {
    case llvm::Instruction::Add:            // fall through...
    case llvm::Instruction::FAdd:           return emitOp<2>(ir_binop_add,     llvmInst);
    case llvm::Instruction::Sub:            // fall through...
    case llvm::Instruction::FSub:           return emitOp<2>(ir_binop_sub,     llvmInst);
    case llvm::Instruction::Mul:            // fall through...
    case llvm::Instruction::FMul:           return emitOp<2>(ir_binop_mul,     llvmInst);
    case llvm::Instruction::UDiv:           // fall through...
    case llvm::Instruction::SDiv:           // fall through...
    case llvm::Instruction::FDiv:           return emitOp<2>(ir_binop_div,     llvmInst);
    case llvm::Instruction::URem:           // fall through...
    case llvm::Instruction::SRem:           // fall through...
    case llvm::Instruction::FRem:           return emitFn("mod", llvmInst);
    case llvm::Instruction::Shl:            return emitOp<2>(ir_binop_lshift,  llvmInst);
    case llvm::Instruction::LShr:           assert(0); break; // LunarG TODO: ...
    case llvm::Instruction::AShr:           assert(0); break; // LunarG TODO: ...
    case llvm::Instruction::And:            return emitOpBit<2>(ir_binop_logic_and, ir_binop_bit_and, llvmInst);
    case llvm::Instruction::Or:             return emitOpBit<2>(ir_binop_logic_or,  ir_binop_bit_or,  llvmInst);
    // LunarG TODO: xor needs a dedicated emitter, to handle all the stuff
    // LLVM does with it.  Like unary negation and whatnot.
    case llvm::Instruction::Xor:            return emitOpBit<2>(ir_binop_logic_xor, ir_binop_bit_xor, llvmInst);
    case llvm::Instruction::ICmp:           // fall through...
    case llvm::Instruction::FCmp:           return emitCmp(llvmInst);
    case llvm::Instruction::FPToUI:         return emitOp<1>(ir_unop_f2u,      llvmInst);
    // LunarG TODO: for ZExt, if we need more than 1 to 32 bit conversions, more
    // smarts wil be needed than blind use of ir_unop_b2i.
    case llvm::Instruction::ZExt:           return emitOp<1>(ir_unop_b2i,      llvmInst);
    case llvm::Instruction::SExt:           return emitIRSext(llvmInst);
    case llvm::Instruction::FPToSI:         return emitOp<1>(ir_unop_f2i,      llvmInst);
    case llvm::Instruction::UIToFP:         return emitOpBit<1>(ir_unop_b2f, ir_unop_u2f, llvmInst);
    case llvm::Instruction::SIToFP:         return emitOp<1>(ir_unop_i2f,      llvmInst);
    case llvm::Instruction::Call:           return emitIRCallOrIntrinsic(llvmInst);
    case llvm::Instruction::Ret:            return emitIRReturn(llvmInst, lastBlock);
    case llvm::Instruction::Load:           return emitIRLoad(llvmInst);
    case llvm::Instruction::Alloca:         return emitIRalloca(llvmInst);
    case llvm::Instruction::Store:          return emitIRStore(llvmInst);
    case llvm::Instruction::ExtractElement: return emitIRExtractElement(llvmInst);
    case llvm::Instruction::InsertElement:  return emitIRInsertElement(llvmInst);
    case llvm::Instruction::Select:         return emitIRSelect(llvmInst);
    case llvm::Instruction::GetElementPtr:  break; // defer until we process the load.
    case llvm::Instruction::ExtractValue:   return emitIRExtractValue(llvmInst);
    case llvm::Instruction::InsertValue:    return emitIRInsertValue(llvmInst);
    case llvm::Instruction::ShuffleVector:  assert(0 && "LunarG TODO: ShuffleVector"); break; // LunarG TODO: ...
       break;

    default:
       assert(0 && "unimplemented");
       // LunarG TODO: report error
    }
}


/**
 * -----------------------------------------------------------------------------
 * Factory for Mesa LunarGLASS back-end translator
 * -----------------------------------------------------------------------------
 */
BackEndTranslator* GetMesaGlassTranslator(Manager* manager)
{
    return new MesaGlassTranslator(manager);
}

/**
 * -----------------------------------------------------------------------------
 * Destroy back-end translator
 * -----------------------------------------------------------------------------
 */
void ReleaseMesaGlassTranslator(BackEndTranslator* target)
{
    delete target;
}

} // namespace gla

#endif // USE_LUNARGLASS