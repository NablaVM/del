#include <cctype>
#include <fstream>
#include <cassert>
#include <iostream>
#include <string>
#include <libnabla/assembler.hpp>
#include "del_driver.hpp"

namespace DEL
{
   DEL_Driver::DEL_Driver() : error_man(*this), 
                              preproc(error_man),
                              symbol_table(error_man, memory_man),
                              code_gen(error_man, symbol_table, memory_man),
                              analyzer(error_man, symbol_table, code_gen, memory_man)
   {
      symbol_table.new_context("global");
   }

   // ----------------------------------------------------------
   //
   // ----------------------------------------------------------

   DEL_Driver::~DEL_Driver()
   {
      delete(scanner);
      scanner = nullptr;
      delete(parser);
      parser = nullptr;
   }

   // ----------------------------------------------------------
   //
   // ----------------------------------------------------------
   
   void DEL_Driver::parse( const char * const filename )
   {
      assert( filename != nullptr );

      // If this fails, things die
      preproc.process(filename);

      std::string preproc_file = preproc.get_preprocessed_filename();

      std::ifstream in_file(preproc_file);
      if( ! in_file.good() )
      {
         std::cerr << "Developer Error: Preprocessed file : " << preproc_file << " unable to be opened" << std::endl;
         exit( EXIT_FAILURE );
      }
      parse_helper( in_file );
      return;
   }

   // ----------------------------------------------------------
   //
   // ----------------------------------------------------------
   
   void DEL_Driver::parse( std::istream &stream )
   {
      if( ! stream.good()  && stream.eof() )
      {
         return;
      }
      //else
      parse_helper( stream ); 
      return;
   }

   // ----------------------------------------------------------
   //
   // ----------------------------------------------------------
   
   void  DEL_Driver::parse_helper( std::istream &stream )
   {
      delete(scanner);
      try
      {
         scanner = new DEL::DEL_Scanner( &stream, (*this) );
      }
      catch( std::bad_alloc &ba )
      {
         std::cerr << "Failed to allocate scanner: (" << ba.what() << ")\n";
         exit( EXIT_FAILURE );
      }

      delete(parser); 
      try
      {
         parser = new DEL::DEL_Parser( (*scanner) /* scanner */, 
                                       (*this)    /* driver  */);
      }
      catch( std::bad_alloc &ba )
      {
         std::cerr << "Failed to allocate parser: (" <<  ba.what() << ")\n";
         exit( EXIT_FAILURE );
      }
      const int accept( 0 );
      if( parser->parse() != accept )
      {
         std::cerr << "Parse failed!!\n";
      }
      return;
   }

   // ----------------------------------------------------------
   //
   // ----------------------------------------------------------
   
   std::ostream& DEL_Driver::print( std::ostream &stream )
   {
      //stream << "done" << "\n";
      return(stream);
   }

   // ----------------------------------------------------------
   //
   // ----------------------------------------------------------

   void DEL_Driver::indicate_complete()
   {
      // Check that the analyzer is okay with us being done
      analyzer.check_for_finalization();

      // Directly tell the code gen we want some ASM
      std::vector<std::string> ASM = code_gen.indicate_complete();

      /*
         TODO : 

         Eventually I would like to move the followign code to a class that handles all the FILE IO and cleanup,
         but for the moment this will do just fine. 
      
      */

      bool assemble_verbose = false;

      std::string asm_output_file = DEFAULT_ASM_OUT;
      std::string bin_output_file = DEFAULT_BIN_OUT;

      // output ASM
      {
         std::ofstream asm_out;
         asm_out.open(asm_output_file);
         if(!asm_out.is_open())
         {
            error_man.report_unable_to_open_result_out(asm_output_file, true);
         }
         for(auto & l : ASM)
         {
            asm_out << l;
         }
         asm_out.close();
         ASM.clear();
      }

      std::vector<uint8_t> binary_data;

      if(!ASSEMBLER::ParseAsm(asm_output_file, binary_data, assemble_verbose))
      {
         error_man.report_custom("DEL::Driver", "Developer Error : Generated ASM code would not assemble", true);
      }

      // output BYTE CODE
      {
         std::ofstream bin_out;
         bin_out.open(bin_output_file);
         if(!bin_out.is_open())
         {
            error_man.report_unable_to_open_result_out(bin_output_file, true);
         }
         for(auto & l : binary_data)
         {
            bin_out << l;
         }
         bin_out.close();
         binary_data.clear();
      }

      // Keep this until we allow configuration of output names / etc
      std::cout << ">>> Complete <<<" << std::endl 
                << "Binary output file : " << DEFAULT_BIN_OUT << std::endl 
                << "Nabla ASM file     : " << DEFAULT_ASM_OUT << std::endl;
   }

   // ----------------------------------------------------------
   //
   // ----------------------------------------------------------

   void DEL_Driver::build_function(Function *function)
   {
      // Trigger the analyzer with a function
      analyzer.build_function(function);
   }

   // ----------------------------------------------------------
   //
   // ----------------------------------------------------------

   void DEL_Driver::preproc_file_directive(std::string directive)
   {
      current_file_from_directive = (directive.substr(6, directive.size()));
   }

   // ----------------------------------------------------------
   //
   // ----------------------------------------------------------

   DEL::Errors & DEL_Driver::get_error_man_ref()
   {
      return error_man;
   }

   // ----------------------------------------------------------
   //
   // ----------------------------------------------------------

   DEL::Preprocessor & DEL_Driver::get_preproc_ref()
   {
      return preproc;
   }

}