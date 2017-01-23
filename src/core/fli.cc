/*
    File: fli.cc
*/

/*
Copyright (c) 2016, Christian E. Schafmeister
Copyright (c) 2016 and beyond, Frank Goenninger

CLASP is free software; you can redistribute it and/or
modify it under the terms of the GNU Library General Public
License as published by the Free Software Foundation; either
version 2 of the License, or (at your option) any later version.

See directory 'clasp/licenses' for full details.

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/
/* -^- */

// ===========================================================================
//
//  FLI - Foreign Language Interface for allocating, setting,
//        getting and freeing foreign memory and calling foreign
//        functions.
//
//  Augiust 2016 by Frank Goenninger, Gönninger B&T UG, Germany
//
// ===========================================================================

// --- IMPLEMEMTATION NOTES ---
//
// The complete FLI is comprised of the following files:
// .../src/core/fli.cc            - this file, main API for FLI
// .../include/clasp/core/fli.h   - corresponding .h file
// .../src/lisp/kernel/fli.lsp    - Lisp land macros and functions
// .../src/llvmo/intrinsics.cc    - translators: to/from Lisp objects
//
// --- END OF IMPLEMEMTATION NOTES ---

// ---------------------------------------------------------------------------
//   SYSTEM INCLUDES
// ---------------------------------------------------------------------------

#include <map>
#include <type_traits>
#include <cstdint>
#include <algorithm>

#include <dlfcn.h>
#include <arpa/inet.h> // for htonl

// ---------------------------------------------------------------------------
//   LLVM INCLUDES
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
//   CLASP INCLUDES
// ---------------------------------------------------------------------------

#define DEBUG_LEVEL_FULL

#include <clasp/core/foundation.h>
#include <clasp/core/lisp.h>
#include <clasp/core/compiler.h>
#include <clasp/core/symbolTable.h>
#include <clasp/core/fli.h>
#include <clasp/core/numbers.h>
#include <clasp/core/character.h>
#include <clasp/core/array.h>
#include <clasp/core/designators.h>
#include <clasp/llvmo/intrinsics.h>

#include <clasp/core/wrappers.h> // last include is wrappers.h

// ---------------------------------------------------------------------------
//   I M P L E M E N T A T I O N
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
//   LOCAL DEFINES
// ---------------------------------------------------------------------------

// frgo, 2016-08-25: The following is part of experieental code...
// #undef MEM_REF_FUNC
// #define MEM_REF_FUNC(CXX_TYPE,CLASP_FN) \
//   void *mem_ref_fn = nullptr; \
//   core::T_sp (*mem_ref_fn)(const cl_intptr_t address)  = []( const cl_intptr_t address ) -> core::T_sp { \
//       return CLASP_FN( * (reinterpret_cast< CXX_TYPE * >( reinterpret_cast< void *>( address ) ) ) ); \
//   };

// This is a hell of a "macro" ... Here are the descriptions of the params:
// TST           => core::VectorObjects_sp - SmartPtr to a symbol holding the
//                                           an array of ForeignTypeSpec_sp
// IDX           => int                    - Numeric index number as an index
//                                           into the TST array
// LISP_SYM_TYPE => :char, :short, ...     - The keyword representing the type
//                                           on Lisp sie
// CXX_SYM_VAR   => char, short, ...       - The C++ variable demoniating the
//                                           Lisp symbol. This will be expanded
//                                           info: kw::_sym_char, kw::_sym:short
// CXX_DESC_STR  => "char", "short", ...   - A string, used as a description
// CLASP_MEM_REF_MAK_OBJ_FN =>               A function name for a fucntion,
//                                           that translates memory content
//                                           into a lisp value.

#define CLASP_CORE_FLI_REGISTER_FOREIGN_TYPE(TST,IDX,LISP_SYM_TYPE,CXX_TYPE,CXX_SYM_VAR,CXX_DESC_STR) \
  { \
    SYMBOL_EXPORT_SC_(KeywordPkg,LISP_SYM_TYPE); \
    register_foreign_type<CXX_TYPE>::doit(TST,IDX,CXX_SYM_VAR,CXX_DESC_STR); \
  }

// ---------------------------------------------------------------------------
//   NAMESPACE
// ---------------------------------------------------------------------------

namespace clasp_ffi {

// ---------------------------------------------------------------------------
//   TYPE DEFINTITIONS
// ---------------------------------------------------------------------------

// None

// ---------------------------------------------------------------------------
//   GLOBAL VARS
// ---------------------------------------------------------------------------

const std::string TO_OBJECT_FN_NAME_PREFIX( "tr_to_object_" );
const std::string FROM_OBJECT_FN_NAME_PREFIX( "tr_from_object_" );

// ---------------------------------------------------------------------------
//   FORWARD DECLARATIONS
// ---------------------------------------------------------------------------

void setup_endianess_info(void);
void register_foreign_types(void);
void register_foreign_type_spec( core::VectorObjects_sp sp_tst,
                                 uint32_t n_index,
                                 const core::Symbol_sp lisp_symbol,
                                 const std::string &lisp_name,
                                 const size_t size,
                                 const size_t alignment,
                                 const std::string &cxx_name);

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
template<typename T, typename... U>
size_t get_fn_address(std::function<T(U...)> f) {
  typedef T(fn_type)(U...);
  fn_type ** fn_ptr = f.template target<fn_type*>();
  return (size_t) *fn_ptr;
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
template <class T>
struct register_foreign_type {
  static void doit( core::VectorObjects_sp sp_tst,
                    uint32_t n_index,
                    core::Symbol_sp lisp_symbol,
                    const std::string &cxx_name ) {

    size_t size = sizeof(T);
    size_t alignment = std::alignment_of<T>::value;

    // For corrent naming we need to replace all '-' by '_' and make
    // sure we're all lowercase (will be done in Lisp) in lisp_name.
    std::string lisp_name = lisp_symbol->symbolNameAsString();
    std::replace( lisp_name.begin(), lisp_name.end(), '-', '_' );

    register_foreign_type_spec( sp_tst,
                                n_index,
                                lisp_symbol,
                                lisp_name,
                                size,
                                alignment,
                                cxx_name );
  };
};

// ---------------------------------------------------------------------------
//   GLOBAL DECLARATIONS AND SPECIAL VARS
// ---------------------------------------------------------------------------

// NONE

// ---------------------------------------------------------------------------
//   FUNCTIONS & METHODS
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
inline void setup_endianess_info(void)
{
  core::List_sp features = cl::_sym_STARfeaturesSTAR->symbolValue();

  if ( htonl(47) == 47 ) {
    // Big Endian
    features = core::Cons_O::create(kw::_sym_big_endian, features);
  } else {
    // Little Endian.
    features = core::Cons_O::create(kw::_sym_little_endian, features);
  }

  cl::_sym_STARfeaturesSTAR->setf_symbolValue(features);
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
void register_foreign_type_spec( core::VectorObjects_sp sp_tst,
                                 uint32_t n_index,
                                 const core::Symbol_sp lisp_symbol,
                                 const std::string& lisp_name,
                                 const size_t size,
                                 const size_t alignment,
                                 const std::string& cxx_name )
{

  std::locale loc = std::locale("C");
  std::string tmp_str;

  tmp_str = TO_OBJECT_FN_NAME_PREFIX + lisp_name;
  std::stringstream to_object_fn_name_stream;
  std::string to_object_fn_name;

  for( auto elem : tmp_str )
    to_object_fn_name_stream << std::tolower( elem, loc );

  to_object_fn_name = to_object_fn_name_stream.str();

  tmp_str = FROM_OBJECT_FN_NAME_PREFIX + lisp_name;
  std::stringstream from_object_fn_name_stream;
  std::string from_object_fn_name;

  for( auto elem : tmp_str )
    from_object_fn_name_stream << std::tolower( elem, loc );

  from_object_fn_name = from_object_fn_name_stream.str();

  ForeignTypeSpec_sp sp_fts =
    ForeignTypeSpec_O::create( lisp_symbol,
                               core::Str_O::create( lisp_name ),
                               core::make_fixnum(size),
                               core::make_fixnum(alignment),
                               core::Str_O::create( cxx_name ),
                               _Nil<core::T_O>(),
                               core::Str_O::create( to_object_fn_name ),
                               core::Str_O::create( from_object_fn_name ),
                               _Nil<core::T_O>(),
                               _Nil<core::T_O>() );

  sp_tst->rowMajorAset( n_index, sp_fts->asSmartPtr() );
};

// ---------------------------------------------------------------------------
//  FOREIGN TYPES REGISTRATION
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
inline void register_foreign_types( void )
{

  uint32_t n_index = 0;

  // STEP 1 : REGISTER FOREIGN TYPES

  core::VectorObjects_sp sp_tst =
    core::VectorObjects_O::make( 64, _Nil<core::T_O>() );

  //  - 1.1 : CREATE FOREIGN TYPE SPECS

  CLASP_CORE_FLI_REGISTER_FOREIGN_TYPE(sp_tst,n_index++,char,char,kw::_sym_char,"char");
  CLASP_CORE_FLI_REGISTER_FOREIGN_TYPE(sp_tst,n_index++,unsigned_char,unsigned char,kw::_sym_unsigned_char,"unsigned char");
  CLASP_CORE_FLI_REGISTER_FOREIGN_TYPE(sp_tst,n_index++,short,short,kw::_sym_short,"short");
  CLASP_CORE_FLI_REGISTER_FOREIGN_TYPE(sp_tst,n_index++,unsigned_short,unsigned short,kw::_sym_unsigned_short,"short");
  CLASP_CORE_FLI_REGISTER_FOREIGN_TYPE(sp_tst,n_index++,int,int,kw::_sym_int,"int");
  CLASP_CORE_FLI_REGISTER_FOREIGN_TYPE(sp_tst,n_index++,unsigned_int,unsigned int,kw::_sym_unsigned_int,"unsigned int");
  CLASP_CORE_FLI_REGISTER_FOREIGN_TYPE(sp_tst,n_index++,long,long,kw::_sym_long,"long");
  CLASP_CORE_FLI_REGISTER_FOREIGN_TYPE(sp_tst,n_index++,unsigned_long,unsigned long,kw::_sym_unsigned_long,"unsigned long");
  CLASP_CORE_FLI_REGISTER_FOREIGN_TYPE(sp_tst,n_index++,long_long,long long,kw::_sym_long_long,"long long");
  CLASP_CORE_FLI_REGISTER_FOREIGN_TYPE(sp_tst,n_index++,unsigned_long_long,unsigned long long,kw::_sym_unsigned_long_long,"unsigned long long");
  CLASP_CORE_FLI_REGISTER_FOREIGN_TYPE(sp_tst,n_index++,unsigned_char,unsigned char,kw::_sym_unsigned_char,"unsigned char");
  CLASP_CORE_FLI_REGISTER_FOREIGN_TYPE(sp_tst,n_index++,uchar,unsigned char,kw::_sym_uchar,"uchar");
  CLASP_CORE_FLI_REGISTER_FOREIGN_TYPE(sp_tst,n_index++,ushort,unsigned short,kw::_sym_ushort,"ushort");
  CLASP_CORE_FLI_REGISTER_FOREIGN_TYPE(sp_tst,n_index++,uint,unsigned int,kw::_sym_uint,"uint");
  CLASP_CORE_FLI_REGISTER_FOREIGN_TYPE(sp_tst,n_index++,ulong,unsigned long,kw::_sym_ulong,"ulong");
  CLASP_CORE_FLI_REGISTER_FOREIGN_TYPE(sp_tst,n_index++,llong,long long,kw::_sym_llong,"llong");
  CLASP_CORE_FLI_REGISTER_FOREIGN_TYPE(sp_tst,n_index++,ullong,unsigned long long,kw::_sym_ullong,"ullong");
  CLASP_CORE_FLI_REGISTER_FOREIGN_TYPE(sp_tst,n_index++,int8,int8_t,kw::_sym_int8,"int8");
  CLASP_CORE_FLI_REGISTER_FOREIGN_TYPE(sp_tst,n_index++,uint8,uint8_t,kw::_sym_uint8,"uint8");
  CLASP_CORE_FLI_REGISTER_FOREIGN_TYPE(sp_tst,n_index++,int16,int16_t,kw::_sym_int16,"int16");
  CLASP_CORE_FLI_REGISTER_FOREIGN_TYPE(sp_tst,n_index++,uint16,uint16_t,kw::_sym_uint16,"uint16");
  CLASP_CORE_FLI_REGISTER_FOREIGN_TYPE(sp_tst,n_index++,int32,int32_t,kw::_sym_int32,"int32");
  CLASP_CORE_FLI_REGISTER_FOREIGN_TYPE(sp_tst,n_index++,uint32,uint32_t,kw::_sym_uint32,"uint32");
  CLASP_CORE_FLI_REGISTER_FOREIGN_TYPE(sp_tst,n_index++,int64,int64_t,kw::_sym_int64,"int64");
  CLASP_CORE_FLI_REGISTER_FOREIGN_TYPE(sp_tst,n_index++,uint64,uint64_t,kw::_sym_uint64,"uint64");
  CLASP_CORE_FLI_REGISTER_FOREIGN_TYPE(sp_tst,n_index++,double,double,kw::_sym_double,"double");
  CLASP_CORE_FLI_REGISTER_FOREIGN_TYPE(sp_tst,n_index++,float,float,kw::_sym_float,"float");
  CLASP_CORE_FLI_REGISTER_FOREIGN_TYPE(sp_tst,n_index++,single_float,float,kw::_sym_single_float,"float");
  CLASP_CORE_FLI_REGISTER_FOREIGN_TYPE(sp_tst,n_index++,long_double,long double,kw::_sym_long_double,"long double");
  CLASP_CORE_FLI_REGISTER_FOREIGN_TYPE(sp_tst,n_index++,time,time_t,kw::_sym_time,"time");
  CLASP_CORE_FLI_REGISTER_FOREIGN_TYPE(sp_tst,n_index++,pointer,void *,kw::_sym_pointer,"pointer");

  CLASP_CORE_FLI_REGISTER_FOREIGN_TYPE(sp_tst,n_index++,size,size_t,kw::_sym_size,"size");
  CLASP_CORE_FLI_REGISTER_FOREIGN_TYPE(sp_tst,n_index++,ssize,ssize_t,kw::_sym_ssize,"ssize");
  CLASP_CORE_FLI_REGISTER_FOREIGN_TYPE(sp_tst,n_index++,ptrdiff,ptrdiff_t,kw::_sym_ptrdiff,"ptrdiff");

  CLASP_CORE_FLI_REGISTER_FOREIGN_TYPE(sp_tst,n_index++,pointer,void *,kw::_sym_void,"void");

  //  - 1.2 : ASSIGN FOREGN TYPE SPEC TABLE TO GLOBAL SYMBOL

  _sym_STARforeign_type_spec_tableSTAR->defparameter( sp_tst );

}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
CL_INITIALIZER void clasp_fli_initialization(void)
{

  // 1 - REGISTER FOREIGN TYPES

  register_foreign_types();

  // 2 - SETUP ENDIANESS INFO

  setup_endianess_info();

  // CLASP FFI INITIALIZATION DONE
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------

//  F O R E I G N   D A T A

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
ForeignData_O::ForeignData_O() : m_kind(_Nil<T_O>()),
                                 m_ownership_flags(core::ForeignDataFlagEnum::None),
                                 m_size(0),
                                 m_orig_data_ptr(nullptr),
                                 m_raw_data(nullptr)
{
  // NOTHIHG TO DO
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
ForeignData_O::~ForeignData_O()
{
  if ( this->m_ownership_flags & core::ForeignDataFlagEnum::DeleteOnDtor ) {
    this->free();
  }
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
inline string ForeignData_O::__repr__() const
{

  stringstream ss;

  ss << "#<"
     << this->_instanceClass()->classNameAsString()
     << " @ " << (BF("%p") % this)
     << " :kind " << this->m_kind
     << " :size " << this->m_size
     << " :ownership-flags " << this->m_ownership_flags
     << " :data-ptr "
     << (BF("%p") % this->m_raw_data)
     << " :orig-ptr "
     << (BF("%p") % this->m_orig_data_ptr)
     << ">";

  return ss.str();
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
void * ForeignData_O::externalObject() const
{
  return this->m_raw_data;
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
bool ForeignData_O::eql_(core::T_sp obj) const
{
  if (core__external_object_p(obj)) {
    return (gc::As<core::ExternalObject_sp>(obj)->externalObject() == this->externalObject());
  }
  return false;
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
void ForeignData_O::allocate(core::T_sp kind, core::ForeignDataFlagEnum ownership_flags, size_t size)
{
  this->m_kind = kind;
  this->m_ownership_flags = ownership_flags;
  this->m_size = size;
  this->m_raw_data = (void *) gctools::clasp_alloc_atomic(size);
  this->m_orig_data_ptr = this->m_raw_data;
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
void ForeignData_O::free()
{
  gctools::clasp_dealloc( (char *)this->m_orig_data_ptr );
  this->m_orig_data_ptr = nullptr;
  this->m_size          = 0;
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
inline bool ForeignData_O::null_pointer_p()
{
  return ( this->raw_data() == nullptr );
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
core::T_sp ForeignData_O::PERCENTkind()
{
  return this->kind();
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
core::Integer_sp ForeignData_O::PERCENTownership_flags() {
  return core::Integer_O::create( (gctools::Fixnum) this->ownership_flags());
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
core::Integer_sp ForeignData_O::PERCENTforeign_data_address() {
  cl_intptr_t result = this->data<cl_intptr_t>();
  return core::Integer_O::create( result );
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
ForeignData_sp PERCENTallocate_foreign_object(core::T_sp kind) {

  size_t n_size = 0;

  if( core::cl__symbolp( kind ) ) {
    n_size = PERCENTforeign_type_size( kind );
  }
  else {
    // Get the size out of kind's v of form (:array n)
    core::Cons_sp ckind = gc::As<core::Cons_sp>(kind);
    if ( ! (oCar(ckind)==cl::_sym_array || oCar(ckind)==kw::_sym_array ) ) {
      SIMPLE_ERROR(BF("The first element of a foreign-data type must be ARRAY or :ARRAY"));
    }
    if (!(oCadr(ckind) == cl::_sym_UnsignedByte || oCadr(ckind) == kw::_sym_UnsignedByte)) {
    SIMPLE_ERROR(BF("The second element of a foreign-data type must be UNSIGNED-BYTE or :UNSIGNED-BYTE"));
    }
    n_size = unbox_fixnum(gc::As<core::Fixnum_sp>(oCaddr(ckind)));
  }

  GC_ALLOCATE(ForeignData_O, self);
  self->allocate( kind, core::DeleteOnDtor, n_size );

  return self;
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
void ForeignData_O::PERCENTfree_foreign_object()
{
  this->free();
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
ForeignData_sp PERCENTallocate_foreign_data(core::Integer_sp size)
{
  size_t _size = unbox_fixnum( size );
  GC_ALLOCATE(ForeignData_O, self);
  self->allocate( kw::_sym_clasp_foreign_data_kind_data, core::DeleteOnDtor, _size);
  return self;
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
void ForeignData_O::PERCENTfree_foreign_data()
{
  this->free();
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
ForeignData_sp ForeignData_O::create(cl_intptr_t address)
{
  GC_ALLOCATE(ForeignData_O, self);
  self->m_raw_data = reinterpret_cast<void *>( address );
  self->set_kind( kw::_sym_clasp_foreign_data_kind_pointer );
  return self;
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
ForeignData_sp ForeignData_O::create(void * p_address)
{
  GC_ALLOCATE(ForeignData_O, self);
  self->m_raw_data = p_address;
  self->set_kind( kw::_sym_clasp_foreign_data_kind_pointer );
  return self;
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
ForeignData_sp make_pointer( void * p_address )
{
  return ForeignData_O::create( p_address );
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
ForeignData_sp PERCENTmake_pointer(core::Integer_sp address)
{
  ForeignData_sp ptr = ForeignData_O::create( core::clasp_to_cl_intptr_t( address ) );
  ptr->set_kind( kw::_sym_clasp_foreign_data_kind_pointer );
  return ptr;
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
ForeignData_sp PERCENTmake_nullpointer()
{
  ForeignData_sp ptr = ForeignData_O::create( (cl_intptr_t) 0 );
  ptr->set_kind( kw::_sym_clasp_foreign_data_kind_pointer );
  return ptr;
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
core::Pointer_sp PERCENTcore_pointer_from_foreign_data( ForeignData_sp fd_ptr )
{
  return core::Pointer_O::create( fd_ptr->ptr() );
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
core::T_sp PERCENTforeign_data_pointerp( core::T_sp obj )
{
  ForeignData_sp sp_foreign_data = obj.asOrNull<ForeignData_O>();

  if( sp_foreign_data.nilp() )
  {
    return _Nil<core::T_O>();
  }
  else
  {
    return _lisp->_true();
  }
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
core::T_sp PERCENTpointerp( core::T_sp obj )
{
  ForeignData_sp sp_foreign_data = obj.asOrNull<ForeignData_O>();

  if( sp_foreign_data.nilp() )
  {
    return _Nil<core::T_O>();
  }
  else
  {
    return _lisp->_true();
  }
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
core::T_sp PERCENTnull_pointer_p( core::T_sp obj )
{
  ForeignData_sp sp_foreign_data = obj.asOrNull<ForeignData_O>();

  if( sp_foreign_data.nilp() )
  {
    return _Nil<core::T_O>();
  }
  else
  {
    if( sp_foreign_data->null_pointer_p() )
    {
      return _lisp->_true();
    }
    else
    {
      return _Nil<core::T_O>();
    }
  }
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
ForeignData_sp ForeignData_O::PERCENTinc_pointer( core::Integer_sp offset)
{
  cl_intptr_t new_address = 0;
  cl_intptr_t raw_data_address = reinterpret_cast<cl_intptr_t>( this->raw_data() );
  cl_intptr_t offset_ = core::clasp_to_cl_intptr_t( offset );

  new_address = raw_data_address + offset_;
  this->m_raw_data  = reinterpret_cast<void *>( new_address );

  return this->asSmartPtr();
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
core::Fixnum_sp PERCENTforeign_type_alignment(core::Symbol_sp atype)
{
  core::Fixnum_sp result = nullptr;

  core::VectorObjects_sp sp_tst = _sym_STARforeign_type_spec_tableSTAR->symbolValue();
  auto iterator = sp_tst->begin();
  auto it_end = sp_tst->end();

  for (; iterator != it_end; iterator++) {
    ForeignTypeSpec_sp sp_fts = iterator->asOrNull<ForeignTypeSpec_O>();
    if ( sp_fts.notnilp() ) {
      if ( sp_fts->PERCENTlisp_symbol()->eql_( atype ) ) {
        result = sp_fts->PERCENTalignment();
        goto RETURN_FROM_CORE__PERCENT_FOREIGN_TYPE_ALIGNMENT;
      }
    }
  }

  SIMPLE_ERROR(BF("No foreign type alignment available for %s") % _rep_(atype));
  return _Nil<core::T_O>();

RETURN_FROM_CORE__PERCENT_FOREIGN_TYPE_ALIGNMENT:

  return result;
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
core::Fixnum_sp PERCENTforeign_type_size(core::Symbol_sp atype)
{
  core::Fixnum_sp result = nullptr;

  core::VectorObjects_sp sp_tst = _sym_STARforeign_type_spec_tableSTAR->symbolValue();
  auto iterator = sp_tst->begin();
  auto it_end = sp_tst->end();

  for (; iterator != it_end; iterator++) {
    ForeignTypeSpec_sp sp_fts = iterator->asOrNull<ForeignTypeSpec_O>();
    if ( sp_fts.notnilp() ) {
      if ( sp_fts->PERCENTlisp_symbol()->eql_( atype ) ) {
        result = sp_fts->PERCENTsize();
        goto RETURN_FROM_CORE__PERCENT_FOREIGN_TYPE_SIZE;
      }
    }
  }

  SIMPLE_ERROR(BF("No foreign type size available for %s") % _rep_(atype));
  return _Nil<core::T_O>();

RETURN_FROM_CORE__PERCENT_FOREIGN_TYPE_SIZE:

  // fprintf( stderr, "*** PERCENTforeign_type_size of %s = %ld\n",
  //          atype->symbolNameAsString().c_str(),
  //          unbox_fixnum( result ) );

  return result;
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
core::T_sp PERCENTdlopen( core::T_sp path_designator )
{
  ForeignData_sp sp_handle;
  int n_mode = RTLD_NOW | RTLD_GLOBAL;

  core::Path_sp path = core::coerce::pathDesignator( path_designator );
  string str_path = path->asString();

  auto result = core::do_dlopen( str_path, n_mode );
  void * p_handle = std::get<0>( result );

  if( p_handle == nullptr ) {
    return ( Values(_Nil<core::T_O>(), core::Str_O::create( get<1>( result ))) );
  }

  sp_handle = ForeignData_O::create( p_handle );
  sp_handle->set_kind( kw::_sym_clasp_foreign_data_kind_dynamic_library );

  return ( Values( sp_handle, _Nil<core::T_O>()) );
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
core::T_sp PERCENTdlclose( ForeignData_sp handle )
{
  auto result = core::do_dlclose( handle->raw_data() );
  int n_rc = std::get<0>( result );

  if( n_rc != 0 ) {
    return ( Values(_Nil<core::T_O>(), core::Str_O::create( get<1>( result ))) );
  }
  return ( Values( _lisp->_true(), _Nil<core::T_O>()) );
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
core::T_sp PERCENTdlsym( core::String_sp name ) {

  ForeignData_sp sp_sym;
  auto result = core::do_dlsym( RTLD_DEFAULT, name->get().c_str() );
  void *p_sym = std::get<0>( result );

  if( ! p_sym ) {
    return ( Values(_Nil<core::T_O>(), core::Str_O::create( get<1>( result ))) );
  }

  sp_sym = ForeignData_O::create( p_sym );
  sp_sym->set_kind( kw::_sym_clasp_foreign_data_kind_symbol_pointer );

  return ( Values( sp_sym, _Nil<core::T_O>()) );
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------

//  F O R E I G N   T Y P E   S P E C

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
ForeignTypeSpec_O::ForeignTypeSpec_O() : m_lisp_symbol( _Nil<T_O>() ),
                                         m_lisp_name( _Nil<T_O>() ),
                                         m_size( (gc::Fixnum) 0 ),
                                         m_alignment( (gc::Fixnum) 0 ),
                                         m_cxx_name( _Nil<T_O>() ),
                                         m_llvm_type_symbol( _Nil<T_O>() ),
                                         m_to_object_fn_name( _Nil<T_O>() ),
                                         m_from_object_fn_name( _Nil<T_O>() ),
                                         m_to_object_fn_ptr( _Nil<T_O>() ),
                                         m_from_object_fn_ptr( _Nil<T_O>() )
{
  // NOTHIHG TO DO
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
ForeignTypeSpec_O::~ForeignTypeSpec_O() {
  // TODO: Do we need to delete ourselves from the foregn type spec table?
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
inline string ForeignTypeSpec_O::__repr__() const {

  stringstream ss;

  ss << "#<"
     << this->_instanceClass()->classNameAsString()
     << " @ " << (BF("%p") % this)
     << " :lisp-symbol " << this->m_lisp_symbol
     << " :lisp-name "   << this->m_lisp_name
     << " :size "        << this->m_size
     << " :alignment "   << this->m_alignment
     << " :cxx-name "    << this->m_cxx_name
     << " :llvm-type-symbol " << this->m_llvm_type_symbol
     << " :to-object-fn-name " << this->m_to_object_fn_name
     << " :from-object-fn-name " << this->m_from_object_fn_name
     << ">";

  return ss.str();
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
ForeignTypeSpec_sp ForeignTypeSpec_O::create( core::Symbol_sp  lisp_symbol,
                                              core::String_sp  lisp_name,
                                              core::Integer_sp size,
                                              core::Fixnum_sp  alignment,
                                              core::String_sp  cxx_name,
                                              core::Symbol_sp  llvm_type_symbol,
                                              core::String_sp  to_object_fn_name,
                                              core::String_sp  from_object_fn_name,
                                              ForeignData_sp   to_object_fn_ptr,
                                              ForeignData_sp   from_object_fn_ptr)
{
  GC_ALLOCATE(ForeignTypeSpec_O, self);

  self->m_lisp_symbol           = lisp_symbol;
  self->m_lisp_name             = lisp_name;
  self->m_size                  = size;
  self->m_alignment             = alignment;
  self->m_cxx_name              = cxx_name;
  self->m_llvm_type_symbol      = llvm_type_symbol;
  self->m_to_object_fn_name     = to_object_fn_name;
  self->m_from_object_fn_name   = from_object_fn_name;
  self->m_to_object_fn_ptr      = to_object_fn_ptr;
  self->m_from_object_fn_ptr    = from_object_fn_ptr;

  return self;
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
bool ForeignTypeSpec_O::eql_(ForeignTypeSpec_sp sp_obj) const {

  if( this->m_lisp_symbol == sp_obj->m_lisp_symbol )
    return true;
  else
    return false;
}

void ForeignTypeSpec_O::PERCENTset_llvm_type_symbol( core::Symbol_sp llvm_type_symbol )
{
  this->m_llvm_type_symbol = llvm_type_symbol;
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------

core::Integer_sp PERCENToffset_address_as_integer( core::T_sp address_or_foreign_data_ptr, core::Integer_sp offset )
{
  cl_intptr_t n_address = 0;
  cl_intptr_t n_offset  = 0;
  cl_intptr_t n_result  = 0;

  // Check if 1st param is NIL -> not allowed!
  if( address_or_foreign_data_ptr.nilp() )
  {
    SIMPLE_ERROR(BF("Invalid parameter type for address!"));
    return _Nil<core::T_O>();
  }

  // If offset is not NIL then get v
  if( offset.notnilp() )
  {
    if( offset.fixnump() )
    {
      n_offset = unbox_fixnum( offset );
    }
    else
    {
      n_offset = core::clasp_to_cl_intptr_t( offset );
    }
  }

  if( address_or_foreign_data_ptr.fixnump() )
  {
    n_address = unbox_fixnum( address_or_foreign_data_ptr );
  }
  else
  {
    ForeignData_sp sp_fd = _Nil<core::T_O>();

    sp_fd = address_or_foreign_data_ptr.asOrNull<ForeignData_O>();
    if( sp_fd.notnilp() && PERCENTpointerp( address_or_foreign_data_ptr ) )
    {
      core::Integer_sp sp_address = _Nil<core::T_O>();

      sp_address = sp_fd->PERCENTforeign_data_address();
      if( sp_address.fixnump() )
      {
        n_address = unbox_fixnum( sp_address );
      }
      else
      {
        n_address = core::clasp_to_cl_intptr_t( sp_address );
      }
    }
    else
    {
      SIMPLE_ERROR(BF("Invalid parameter type for address!"));
      return _Nil<core::T_O>();
    }
  }

  n_result = n_address + n_offset;

  return core::Integer_O::create( n_result );
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
template<class T>
inline T mem_ref( cl_intptr_t address )
{
  T *ptr = reinterpret_cast< T*>( address );
  return (*ptr);
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// MEM-REF

core::T_sp PERCENTmem_ref_short( core::Integer_sp address )
{
  short v = mem_ref<short>( core::clasp_to_cl_intptr_t( address ) );
  return mk_fixnum_short( v );
}

core::T_sp PERCENTmem_ref_unsigned_short( core::Integer_sp address )
{
  unsigned short v = mem_ref<unsigned short>( core::clasp_to_cl_intptr_t( address ) );
  return mk_fixnum_ushort( v );
}

core::T_sp PERCENTmem_ref_int( core::Integer_sp address )
{
  int v = mem_ref<int>( core::clasp_to_cl_intptr_t( address ) );
  return mk_fixnum_int( v );
}

core::T_sp PERCENTmem_ref_unsigned_int( core::Integer_sp address )
{
  unsigned int v = mem_ref<unsigned int>( core::clasp_to_cl_intptr_t( address ) );
  return mk_fixnum_uint( v );
}

core::T_sp PERCENTmem_ref_int8( core::Integer_sp address )
{
  int8_t v = mem_ref<int8_t>( core::clasp_to_cl_intptr_t( address ) );
  return mk_fixnum_int8( v );
}

core::T_sp PERCENTmem_ref_uint8( core::Integer_sp address )
{
  int8_t v = mem_ref<int8_t>( core::clasp_to_cl_intptr_t( address ) );
  return mk_fixnum_uint8( v );
}

core::T_sp PERCENTmem_ref_int16( core::Integer_sp address )
{
  int16_t v = mem_ref<int16_t>( core::clasp_to_cl_intptr_t( address ) );
  return mk_fixnum_int16( v );
}

core::T_sp PERCENTmem_ref_uint16( core::Integer_sp address )
{
  uint16_t v = mem_ref<uint16_t>( core::clasp_to_cl_intptr_t( address ) );
  return mk_fixnum_uint16( v );
}

core::T_sp PERCENTmem_ref_int32( core::Integer_sp address )
{
  int32_t v = mem_ref<int32_t>( core::clasp_to_cl_intptr_t( address ) );
  return mk_fixnum_int32( v );
}

core::T_sp PERCENTmem_ref_uint32( core::Integer_sp address )
{
  uint32_t v = mem_ref<uint32_t>( core::clasp_to_cl_intptr_t( address ) );
  return mk_fixnum_uint32( v );
}

core::T_sp PERCENTmem_ref_int64( core::Integer_sp address )
{
  int64_t v = mem_ref<int64_t>( core::clasp_to_cl_intptr_t( address ) );
  return mk_integer_int64( v );
}

core::T_sp PERCENTmem_ref_uint64( core::Integer_sp address )
{
  uint64_t v = mem_ref<uint64_t>( core::clasp_to_cl_intptr_t( address ) );
  return mk_integer_uint64( v );
}

core::T_sp PERCENTmem_ref_long( core::Integer_sp address )
{
  long v = mem_ref<long>( core::clasp_to_cl_intptr_t( address ) );
  return mk_integer_long( v );
}

core::T_sp PERCENTmem_ref_unsigned_long( core::Integer_sp address )
{
  unsigned long v = mem_ref<unsigned long>( core::clasp_to_cl_intptr_t( address ) );
  return mk_integer_ulong( v );
}

core::T_sp PERCENTmem_ref_long_long( core::Integer_sp address )
{
  long long v = mem_ref<long long>( core::clasp_to_cl_intptr_t( address ) );
  return mk_integer_longlong( v );
}

core::T_sp PERCENTmem_ref_unsigned_long_long( core::Integer_sp address )
{
  unsigned long long v = mem_ref<unsigned long long>( core::clasp_to_cl_intptr_t( address ) );
  return mk_integer_ulonglong( v );
}

core::T_sp PERCENTmem_ref_double( core::Integer_sp address )
{
  double v = mem_ref<double>( core::clasp_to_cl_intptr_t( address ) );
  return mk_double_float( v );
}

core::T_sp PERCENTmem_ref_float( core::Integer_sp address )
{
  float v = mem_ref<float>( core::clasp_to_cl_intptr_t( address ) );
  return mk_single_float( v );
}

core::T_sp PERCENTmem_ref_long_double( core::Integer_sp address )
{
  long double v = mem_ref<long double>( core::clasp_to_cl_intptr_t( address ) );
  return mk_long_double( v );
}

core::T_sp PERCENTmem_ref_time( core::Integer_sp address )
{
  time_t v = mem_ref<time_t>( core::clasp_to_cl_intptr_t( address ) );
  return mk_time( v );
}

core::T_sp PERCENTmem_ref_pointer( core::Integer_sp address )
{
  ForeignData_sp ptr = PERCENTmake_pointer( address );
  return ptr;
}

core::T_sp PERCENTmem_ref_size( core::Integer_sp address )
{
  size_t v = mem_ref<size_t>( core::clasp_to_cl_intptr_t( address ) );
  return mk_size( v );
}

core::T_sp PERCENTmem_ref_ssize( core::Integer_sp address )
{
  ssize_t v = mem_ref<ssize_t>( core::clasp_to_cl_intptr_t( address ) );
  return mk_ssize( v );
}

core::T_sp PERCENTmem_ref_ptrdiff( core::Integer_sp address )
{
  ptrdiff_t v = mem_ref<ptrdiff_t>( core::clasp_to_cl_intptr_t( address ) );
  return mk_ptrdiff( v );
}

core::T_sp PERCENTmem_ref_char( core::Integer_sp address )
{
  char v = mem_ref<char>( core::clasp_to_cl_intptr_t( address ) );
  return mk_char( v );
}

core::T_sp PERCENTmem_ref_unsigned_char( core::Integer_sp address )
{
  unsigned char v = mem_ref<unsigned char>( core::clasp_to_cl_intptr_t( address ) );
  return mk_char( v );
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------

// Lisp to C++ translation, used by mem_set()

// inline ptrdiff_t clasp_to_ptrdiff( core::T_sp sp_lisp_value ) {
//   translate::from_object< ptrdiff_t > v( sp_lisp_value );
//   return v._v;
// }

// inline char clasp_to_char( core::T_sp sp_lisp_value ) {
//   translate::from_object< char > v( sp_lisp_value );
//   return v._v;
// }

void * clasp_to_void_pointer( ForeignData_sp sp_lisp_value )
{
  return sp_lisp_value->ptr();
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
template<typename T>
inline T mem_set( cl_intptr_t address, T value )
{
  * (reinterpret_cast< T * >( address )) = value;
  return * (reinterpret_cast< T * >( address ));
}
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// MEM-SET

core::T_sp PERCENTmem_set_short( core::Integer_sp address, core::T_sp value )
{
  short tmp;
  translate::from_object< short > v( value );
  tmp = mem_set< short >( core::clasp_to_cl_intptr_t( address ), v._v );
  return mk_fixnum_short( tmp );
}

core::T_sp PERCENTmem_set_unsigned_short( core::Integer_sp address, core::T_sp value )
{
  unsigned short tmp;
  translate::from_object< unsigned short > v( value );
  tmp = mem_set< unsigned short >( core::clasp_to_cl_intptr_t( address ), v._v );
  return mk_fixnum_ushort( tmp );
}

core::T_sp PERCENTmem_set_int( core::Integer_sp address, core::T_sp value )
{
  int tmp;
  translate::from_object< int > v( value );
  tmp = mem_set< int >( core::clasp_to_cl_intptr_t( address ), v._v );
  return mk_fixnum_int( tmp );
}

core::T_sp PERCENTmem_set_unsigned_int( core::Integer_sp address, core::T_sp value )
{
  unsigned int tmp;
  translate::from_object< unsigned int > v( value );
  tmp = mem_set< int >( core::clasp_to_cl_intptr_t( address ), v._v );
  return mk_fixnum_uint( tmp );
}

core::T_sp PERCENTmem_set_int8( core::Integer_sp address, core::T_sp value )
{
  int8_t tmp;
  translate::from_object< int8_t > v( value );
  tmp = mem_set< int8_t >( core::clasp_to_cl_intptr_t( address ), v._v );
  return mk_fixnum_int8( tmp );
}

core::T_sp PERCENTmem_set_uint8( core::Integer_sp address, core::T_sp value )
{
  uint8_t tmp;
  translate::from_object< uint8_t > v( value );
  tmp = mem_set< uint8_t >( core::clasp_to_cl_intptr_t( address ), v._v );
  return mk_fixnum_uint8( tmp );
}

core::T_sp PERCENTmem_set_int16( core::Integer_sp address, core::T_sp value )
{
  int16_t tmp;
  translate::from_object< int16_t > v( value );
  tmp = mem_set< int16_t >( core::clasp_to_cl_intptr_t( address ), v._v );
  return mk_fixnum_int16( tmp );
}

core::T_sp PERCENTmem_set_uint16( core::Integer_sp address, core::T_sp value )
{
  uint16_t tmp;
  translate::from_object< uint16_t > v( value );
  tmp = mem_set< uint16_t >( core::clasp_to_cl_intptr_t( address ), v._v );
  return mk_fixnum_uint16( tmp );
}

core::T_sp PERCENTmem_set_int32( core::Integer_sp address, core::T_sp value )
{
  int32_t tmp;
  translate::from_object< int32_t > v( value );
  tmp = mem_set< int32_t >( core::clasp_to_cl_intptr_t( address ), v._v );
  return mk_fixnum_int32( tmp );
}

core::T_sp PERCENTmem_set_uint32( core::Integer_sp address, core::T_sp value )
{
  uint32_t tmp;
  translate::from_object< uint32_t > v( value );
  tmp = mem_set< uint32_t >( core::clasp_to_cl_intptr_t( address ), v._v );
  return mk_fixnum_uint32( tmp );
}

core::T_sp PERCENTmem_set_int64( core::Integer_sp address, core::T_sp value )
{
  int64_t tmp;
  translate::from_object< int64_t > v( value );
  tmp = mem_set< int64_t >( core::clasp_to_cl_intptr_t( address ), v._v );
  return mk_integer_int64( tmp );
}

core::T_sp PERCENTmem_set_uint64( core::Integer_sp address, core::T_sp value )
{
  uint64_t tmp;
  translate::from_object< uint64_t > v( value );
  tmp = mem_set< uint64_t >( core::clasp_to_cl_intptr_t( address ), v._v );
  return mk_integer_uint64( tmp );
}

core::T_sp PERCENTmem_set_long( core::Integer_sp address, core::T_sp value )
{
  long tmp;
  translate::from_object< long > v( value );
  tmp = mem_set< long >( core::clasp_to_cl_intptr_t( address ), v._v );
  return mk_integer_long( tmp );
}

core::T_sp PERCENTmem_set_unsigned_long( core::Integer_sp address, core::T_sp value )
{
  unsigned long tmp;
  translate::from_object< unsigned long > v( value );
  tmp = mem_set< unsigned long >( core::clasp_to_cl_intptr_t( address ), v._v );
  return mk_integer_ulong( tmp );
}

core::T_sp PERCENTmem_set_long_long( core::Integer_sp address, core::T_sp value )
{
  long long tmp;
  translate::from_object< long long > v( value );
  tmp = mem_set< long long >( core::clasp_to_cl_intptr_t( address ), v._v );
  return mk_integer_longlong( tmp );
}

core::T_sp PERCENTmem_set_unsigned_long_long( core::Integer_sp address, core::T_sp value )
{
  unsigned long long tmp;
  translate::from_object< unsigned long long > v( value );
  tmp = mem_set< unsigned long long >( core::clasp_to_cl_intptr_t( address ), v._v );
  return mk_integer_ulonglong( tmp );
}

core::T_sp PERCENTmem_set_double( core::Integer_sp address, core::T_sp value )
{
  double tmp;
  translate::from_object< double > v( value );
  tmp = mem_set< double >( core::clasp_to_cl_intptr_t( address ), v._v );
  return mk_double_float( tmp );
}

core::T_sp PERCENTmem_set_float( core::Integer_sp address, core::T_sp value )
{
  float tmp;
  translate::from_object< float > v( value );
  tmp = mem_set< float >( core::clasp_to_cl_intptr_t( address ), v._v );
  return mk_single_float( tmp );
}

core::T_sp PERCENTmem_set_long_double( core::Integer_sp address, core::T_sp value )
{
  long double tmp;
  translate::from_object< long double > v( value );
  tmp = mem_set< long double >( core::clasp_to_cl_intptr_t( address ), v._v );
  return mk_long_double( tmp );
}

core::T_sp PERCENTmem_set_time( core::Integer_sp address, core::T_sp value )
{
  time_t tmp;
  translate::from_object< time_t > v( value );
  tmp = mem_set< time_t >( core::clasp_to_cl_intptr_t( address ), v._v );
  return mk_time( tmp );
}

core::T_sp PERCENTmem_set_pointer( core::Integer_sp address, core::T_sp value ) {
  IMPLEMENT_ME();
}

core::T_sp PERCENTmem_set_size( core::Integer_sp address, core::T_sp value )
{
  size_t tmp;
  translate::from_object< size_t > v( value );
  tmp = mem_set< size_t >( core::clasp_to_cl_intptr_t( address ), v._v );
  return mk_size( tmp );
}

core::T_sp PERCENTmem_set_ssize( core::Integer_sp address, core::T_sp value )
{
  ssize_t tmp;
  translate::from_object< ssize_t > v( value );
  tmp = mem_set< ssize_t >( core::clasp_to_cl_intptr_t( address ), v._v );
  return mk_ssize( tmp );
}

core::T_sp PERCENTmem_set_ptrdiff( core::Integer_sp address, core::T_sp value )
{
  ptrdiff_t tmp;
  translate::from_object< ptrdiff_t > v( value );
  tmp = mem_set< ptrdiff_t >( core::clasp_to_cl_intptr_t( address ), v._v );
  return mk_ptrdiff( tmp );
}

core::T_sp PERCENTmem_set_char( core::Integer_sp address, core::T_sp value )
{
  char tmp;
  translate::from_object< char > v( value );
  tmp = mem_set< char >( core::clasp_to_cl_intptr_t( address ), v._v );
  return mk_char( tmp );
}

core::T_sp PERCENTmem_set_unsigned_char( core::Integer_sp address, core::T_sp value )
{
  unsigned char tmp;
  translate::from_object< unsigned char > v( value );
  tmp = mem_set< unsigned char >( core::clasp_to_cl_intptr_t( address ), v._v );
  return mk_char( tmp );
}

}; // namespace clasp_ffi

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// T E S T I N G

#include <limits>

unsigned int CLASP_FLI_TEST_MEM_REF_UNSIGNED_INT = 42;
int          CLASP_FLI_TEST_MEM_REF_INT          = -42;
time_t       CLASP_FLI_TEST_MEM_REF_TIME_T       = time( nullptr );
char         CLASP_FLI_TEST_MEM_REF_CHAR         = 'f';

extern "C" void info_numeric_limits( void )
{
  std::cerr << "=============================================================================\n";
  std::cerr << "SHORT MIN: " << std::numeric_limits< short >::min() << "\n";
  std::cerr << "SHORT MAX: " << std::numeric_limits< short >::max() << "\n";

  std::cerr << "INT MIN: " << std::numeric_limits< int >::min() << "\n";
  std::cerr << "INT MAX: " << std::numeric_limits< int >::max() << "\n";

  std::cerr << "LONG MIN: " << std::numeric_limits< long >::min() << "\n";
  std::cerr << "LONG MAX: " << std::numeric_limits< long >::max() << "\n";

  std::cerr << "LONG LONG MIN: " << std::numeric_limits< long long >::min() << "\n";
  std::cerr << "LONG LONG MAX: " << std::numeric_limits< long long >::max() << "\n";

  std::cerr << "FLOAT MIN: " << std::numeric_limits< float >::min() << "\n";
  std::cerr << "FLOAT MAX: " << std::numeric_limits< float >::max() << "\n";

  std::cerr << "DOUBLE MIN: " << std::numeric_limits< double >::min() << "\n";
  std::cerr << "DOUBLE MAX: " << std::numeric_limits< double >::max() << "\n";

  std::cerr << "LONG DOUBLE MIN: " << std::numeric_limits< long double >::min() << "\n";
  std::cerr << "LONG DOUBLE MAX: " << std::numeric_limits< long double >::max() << "\n";
  std::cerr << "=============================================================================\n";
}

extern "C" int fli_test_add( int a, short b )
{
  return a + b;
}

extern "C" char * fli_test_echo_string( char * pc_str )
{
  return pc_str;
}

extern "C" uint32_t fli_test_mul2_uint32(uint32_t x)
{
  return 2 * x;
}

extern "C" long fli_test_mul2_long(long x)
{
  return 2 * x;
}

extern "C" long long fli_test_mul2_long_long(long long x)
{
  return 2 * x;
}
