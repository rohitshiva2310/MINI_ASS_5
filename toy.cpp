#include "ast.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;

// The lexer now lives in lexer.l (flex), the parser in parser.y (bison), and
// the AST classes in ast.h. This file only holds code generation and the
// driver.

int yyparse();

//===----------------------------------------------------------------------===//
// Code Generation
//===----------------------------------------------------------------------===//

static std::unique_ptr<LLVMContext> TheContext;
static std::unique_ptr<Module> TheModule;
static std::unique_ptr<IRBuilder<>> Builder;

// Symbol table: name -> (LLVM value [the argument's alloca], language type).
static std::map<std::string, std::pair<Value *, datatype>> NamedValues;

Value *LogErrorV(const char *Str) {
  fprintf(stderr, "Error: %s\n", Str);
  return nullptr;
}

// Map a language type to its LLVM type:
//   int_type -> i32, bool_type -> i1, double_type -> double.
static Type *getLLVMType(datatype T) {
  switch (T) {
  case int_type:
    return Type::getInt32Ty(*TheContext);
  case bool_type:
    return Type::getInt1Ty(*TheContext);
  case double_type:
    return Type::getDoubleTy(*TheContext);
  }
  return nullptr;
}

// Convert V from type From to type To. Only int <-> double is implicit;
// anything else is reported as an error.
static Value *CastValue(Value *V, datatype From, datatype To) {
  if (From == To)
    return V;
  if (From == int_type && To == double_type)
    return Builder->CreateSIToFP(V, getLLVMType(To), "inttodbl");
  if (From == double_type && To == int_type)
    return Builder->CreateFPToSI(V, getLLVMType(To), "dbltoint");
  return LogErrorV("Invalid type conversion");
}

Value *NumberExprAST::codegen() {
  if (ExprType == int_type)
    return ConstantInt::get(Type::getInt32Ty(*TheContext), (int)Val, true);
  if (ExprType == bool_type)
    return ConstantInt::get(Type::getInt1Ty(*TheContext), Val != 0);
  return ConstantFP::get(*TheContext, APFloat(Val));
}

Value *VariableExprAST::codegen() {
  // Look this variable up in the function.
  auto It = NamedValues.find(Name);
  if (It == NamedValues.end() || !It->second.first)
    return LogErrorV("Unknown variable name");

  Value *Slot = NamedValues[Name].first;   // the alloca holding the value
  datatype Ty = NamedValues[Name].second;  // the saved type
  ExprType = Ty;
  return Builder->CreateLoad(getLLVMType(Ty), Slot, Name.c_str());
}

Value *BinaryExprAST::codegen() {
  Value *L = LHS->codegen();
  Value *R = RHS->codegen();
  if (!L || !R)
    return nullptr;

  datatype LT = LHS->ExprType, RT = RHS->ExprType;

  if (LT == bool_type || RT == bool_type)
    return LogErrorV("Binary operators are not valid on bool operands");

  // Promote to double if either side is double, otherwise stay int.
  datatype OpType =
      (LT == double_type || RT == double_type) ? double_type : int_type;
  if (Op == '/')
    OpType = double_type; // '/' always uses CreateFDiv

  L = CastValue(L, LT, OpType);
  R = CastValue(R, RT, OpType);
  if (!L || !R)
    return nullptr;

  bool IsFP = (OpType == double_type);

  switch (Op) {
  case '+':
    ExprType = OpType;
    return IsFP ? Builder->CreateFAdd(L, R, "addtmp")
                : Builder->CreateAdd(L, R, "addtmp");
  case '-':
    ExprType = OpType;
    return IsFP ? Builder->CreateFSub(L, R, "subtmp")
                : Builder->CreateSub(L, R, "subtmp");
  case '*':
    ExprType = OpType;
    return IsFP ? Builder->CreateFMul(L, R, "multmp")
                : Builder->CreateMul(L, R, "multmp");
  case '/':
    ExprType = double_type;
    return Builder->CreateFDiv(L, R, "divtmp");
  case '<':
    ExprType = bool_type;
    return IsFP ? Builder->CreateFCmpULT(L, R, "cmpltmp")
                : Builder->CreateICmpSLT(L, R, "cmpltmp");
  case '>':
    ExprType = bool_type;
    return IsFP ? Builder->CreateFCmpOGT(L, R, "cmpgtmp")
                : Builder->CreateICmpSGT(L, R, "cmpgtmp");
  default:
    return LogErrorV("invalid binary operator");
  }
}

Value *CallExprAST::codegen() {
  // Look up the name in the global module table.
  Function *CalleeF = TheModule->getFunction(Callee);
  if (!CalleeF)
    return LogErrorV("Unknown function referenced");

  // If argument mismatch error.
  if (CalleeF->arg_size() != Args.size())
    return LogErrorV("Incorrect # arguments passed");

  std::vector<Value *> ArgsV;
  for (unsigned i = 0, e = Args.size(); i != e; ++i) {
    Value *A = Args[i]->codegen();
    if (!A)
      return nullptr;

    // Cast to the parameter's declared type if it differs.
    Type *PT = CalleeF->getFunctionType()->getParamType(i);
    datatype Want = double_type;
    if (PT->isDoubleTy())
      Want = double_type;
    else if (PT->isIntegerTy(1))
      Want = bool_type;
    else
      Want = int_type;

    A = CastValue(A, Args[i]->ExprType, Want);
    if (!A)
      return nullptr;
    ArgsV.push_back(A);
  }

  ExprType = double_type; // functions return double
  return Builder->CreateCall(CalleeF, ArgsV, "calltmp");
}

Function *PrototypeAST::codegen() {
  // Each parameter's LLVM type comes from the datatype in its pair.
  std::vector<Type *> ParamTys;
  for (auto &A : getArgs()) // A.first = name, A.second = datatype
    ParamTys.push_back(getLLVMType(A.second));

  FunctionType *FT =
      FunctionType::get(Type::getDoubleTy(*TheContext), ParamTys, false);

  Function *F =
      Function::Create(FT, Function::ExternalLinkage, getName(), TheModule.get());

  // Set names for all arguments.
  unsigned Idx = 0;
  for (auto &Arg : F->args())
    Arg.setName(getArgs()[Idx++].first);

  return F;
}

Function *FunctionAST::codegen() {
  // First, check for an existing function from a previous 'extern' declaration.
  Function *TheFunction = TheModule->getFunction(Proto->getName());

  if (!TheFunction)
    TheFunction = Proto->codegen();

  if (!TheFunction)
    return nullptr;

  if (!TheFunction->empty())
    return (Function *)LogErrorV("Function cannot be redefined.");

  // Create a new basic block to start insertion into.
  BasicBlock *BB = BasicBlock::Create(*TheContext, "entry", TheFunction);
  Builder->SetInsertPoint(BB);

  // Allocate one slot per argument, typed from the (name, datatype) pairs,
  // and record (alloca, type) in the symbol table.
  NamedValues.clear();
  const auto &ProtoArgs = Proto->getArgs();
  unsigned Idx = 0;
  for (auto &Arg : TheFunction->args()) {
    const std::string &ArgName = ProtoArgs[Idx].first; // name
    datatype ArgType = ProtoArgs[Idx].second;          // type
    ++Idx;

    // i32 for int_type, i1 for bool_type, double for double_type
    AllocaInst *Slot =
        Builder->CreateAlloca(getLLVMType(ArgType), nullptr, ArgName);
    Builder->CreateStore(&Arg, Slot);

    NamedValues[ArgName] = std::make_pair((Value *)Slot, ArgType);
  }

  if (Value *RetVal = Body->codegen()) {
    // The function returns double: bool results are converted with UIToFP,
    // int results with SIToFP.
    if (Body->ExprType == bool_type) {
      RetVal = Builder->CreateUIToFP(RetVal, Type::getDoubleTy(*TheContext),
                                     "booltmp");
    } else {
      RetVal = CastValue(RetVal, Body->ExprType, double_type);
      if (!RetVal) {
        TheFunction->eraseFromParent();
        return nullptr;
      }
    }

    // Finish off the function.
    Builder->CreateRet(RetVal);

    // Validate the generated code, checking for consistency.
    verifyFunction(*TheFunction);

    return TheFunction;
  }

  // Error reading body, remove function.
  TheFunction->eraseFromParent();
  return nullptr;
}


//===----------------------------------------------------------------------===//
// Top-Level handlers and driver
//===----------------------------------------------------------------------===//
// The parser calls these as soon as it reduces a complete top-level item, so
// the REPL still prints IR after each definition / extern / expression.

static void InitializeModule() {
  // Open a new context and module.
  TheContext = std::make_unique<LLVMContext>();
  TheModule = std::make_unique<Module>("my cool jit", *TheContext);

  // Create a new builder for the module.
  Builder = std::make_unique<IRBuilder<>>(*TheContext);
}

void HandleDefinition(std::unique_ptr<FunctionAST> FnAST) {
  if (auto *FnIR = FnAST->codegen()) {
    fprintf(stderr, "Read function definition:");
    FnIR->print(errs());
    fprintf(stderr, "\n");
  }
}

void HandleExtern(std::unique_ptr<PrototypeAST> ProtoAST) {
  if (auto *FnIR = ProtoAST->codegen()) {
    fprintf(stderr, "Read extern: ");
    FnIR->print(errs());
    fprintf(stderr, "\n");
  }
}

void HandleTopLevelExpression(std::unique_ptr<ExprAST> E) {
  // Evaluate a top-level expression into an anonymous function.
  auto Proto = std::make_unique<PrototypeAST>(
      "__anon_expr", std::vector<std::pair<std::string, datatype>>());
  auto FnAST = std::make_unique<FunctionAST>(std::move(Proto), std::move(E));
  if (auto *FnIR = FnAST->codegen()) {
    fprintf(stderr, "Read top-level expression:");
    FnIR->print(errs());
    fprintf(stderr, "\n");

    // Remove the anonymous expression.
    FnIR->eraseFromParent();
  }
}

//===----------------------------------------------------------------------===//
// Main driver code.
//===----------------------------------------------------------------------===//

int main() {
  // Make the module, which holds all the code.
  InitializeModule();

  // Run the parser; it drives codegen through the Handle* callbacks.
  fprintf(stderr, "ready> ");
  yyparse();

  // Print out all of the generated code.
  TheModule->print(errs(), nullptr);

  return 0;
}