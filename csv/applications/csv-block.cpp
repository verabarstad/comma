// This file is part of comma, a generic and flexible library
// Copyright (c) 2011 The University of Sydney
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
// 1. Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
// 3. Neither the name of the University of Sydney nor the
//    names of its contributors may be used to endorse or promote products
//    derived from this software without specific prior written permission.
//
// NO EXPRESS OR IMPLIED LICENSES TO ANY PARTY'S PATENT RIGHTS ARE
// GRANTED BY THIS LICENSE.  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT
// HOLDERS AND CONTRIBUTORS \"AS IS\" AND ANY EXPRESS OR IMPLIED
// WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
// BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
// WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
// OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
// IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

/// @author vsevolod vlaskine

#include <string.h>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include <boost/array.hpp>
#include <boost/functional/hash.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/unordered_map.hpp>
#include <boost/graph/graph_concepts.hpp>
#include <comma/application/command_line_options.h>
#include <comma/application/contact_info.h>
#include <comma/application/signal_flag.h>
#include <comma/base/types.h>
#include <comma/csv/stream.h>
#include <comma/csv/impl/unstructured.h>
#include <comma/io/stream.h>
#include <comma/string/string.h>
#include <comma/visiting/traits.h>

#include <stdio.h>


#include "details/inputs.h"

static const char* name() { return "csv-block: "; }

static void usage( bool more )
{
    std::cerr << std::endl;
    std::cerr << "operations" << std::endl;
    std::cerr << std::endl;
    std::cerr << "head" << std::endl;
    std::cerr << "  cat something.csv | csv-update head --number-of-blocks=1" << std::endl;
    std::cerr << "      reads data to stdout by the number of blocks" << std::endl;
    std::cerr << "      appends indexing field for records in each block" << std::endl;
    std::cerr << "append" << std::endl;
    std::cerr << "  cat something.csv | csv-update append --fields=,group, " << std::endl;
    std::cerr << "      appends block field base on specified id keys" << std::endl;
    std::cerr << "index" << std::endl;
    std::cerr << "  cat something.csv | csv-update append --fields=,block --index" << std::endl;
    std::cerr << "      appends block field, and block's indexing fields" << std::endl;
    std::cerr << "options:" << std::endl;
    std::cerr << "    --help,-h: help; --help --verbose: more help" << std::endl;
    std::cerr << "    --no-reverse; use with 'index' operation, output the indices in ascending order instead of descending" << std::endl;
    std::cerr << std::endl;
    std::cerr << std::endl;
    std::cerr << "contact info: " << comma::contact_info <<std::endl;

    exit(0);
}

static bool verbose;
static comma::csv::options csv;
static comma::signal_flag is_shutdown;
static comma::uint32 block = 0;
static input_t default_input;
static input_t default_output;
static comma::csv::options csv_out;
static bool reverse_index = true;
// All the data for this block
static std::vector< input_t > block_records;

comma::csv::output_stream< input_t >& get_ostream()
{
//     if( verbose ) { std::cerr << name() << "out fields: " << csv_out.fields << std::endl; }
    static comma::csv::output_stream< input_t > ostream( std::cout, csv_out, default_output );
    return ostream;
}

void flush_indexing( std::vector< input_t >& block_records )
{
    comma::csv::output_stream< input_t >& ostream = get_ostream();
    std::size_t size = block_records.size();
    for( std::size_t i=0; i<size; ++i ) 
    {
        input_t record = block_records[i];
        record.value.longs.push_back( reverse_index ? size - (i+1)  : i );
        ostream.write( record );
    }
    block_records.clear();
}

void output_with_appened_index( const input_t& v )
{
    if( block != v.block )
    {
        flush_indexing( block_records );
    }
    block = v.block;
    block_records.push_back( v );
}

void output_from_stdin_head( const input_t& v )
{
    static comma::csv::output_stream< input_t> ostream( std::cout, csv, default_input );
    ostream.write( v );
//     std::cerr << "value: " << v.value.strings.front() << std::endl;
//     if( v.block == 0 ) { std::cin.clear(); fflush( 0 ); exit(0); }
    if( v.block == 0 ) { exit(0); }
}

enum op_type { block_indexing, head_read, block_append }; 

int main( int ac, char** av )
{
    try
    {
        comma::command_line_options options( ac, av, usage );
        verbose = options.exists( "--verbose,-v" );
        csv = comma::csv::options( options );
        csv.full_xpath = true;
        csv.quote.reset();
        
        csv_out = csv;
        
        std::vector< std::string > unnamed = options.unnamed( "--help,-h,--verbose,-v", "-.*" );
        if( unnamed.size() < 1 ) { std::cerr << name() << "expected one operation, got " << comma::join( unnamed, ' ' ) << std::endl; return 1; }
        const std::string  operation = unnamed.front();
        
        std::vector< std::string > v = comma::split( csv.fields, ',' );
        bool has_value_fields = false;
        for( std::size_t i = 0; !has_value_fields && i < v.size(); has_value_fields = !v[i].empty() && v[i] != "block" &&  v[i] != "id", ++i );
        std::string first_line;
        comma::csv::format f;
        if( csv.binary() ) { f = csv.format(); }
        else if( options.exists( "--format" ) ) { f = comma::csv::format( options.value< std::string >( "--format" ) ); }
        else
        {
            while( std::cin.good() && first_line.empty() ) { std::getline( std::cin, first_line ); }
            if( first_line.empty() ) { std::cerr << name() << "--format= is missing however the first line is empty" ; return 1; }
            f = comma::csv::impl::unstructured::guess_format( first_line, csv.delimiter );
            if( verbose ) { std::cerr << name() << "guessed format: " << f.string() << std::endl; }
        }
        bool has_block = false;
        unsigned int size = f.count();
        for( std::size_t i = 0; i < size; ++i )
        {
            if( i < v.size() )
            {
                if( v[i] == "block" ) { has_block = true; continue; }
                if( v[i] == "id" ) { v[i] = "key/" + default_input.key.append( f.offset( i ).type ); continue; }
            }
            if( !has_value_fields || !v[i].empty() )
            {
                v.resize( size );
                v[i] = "value/" + default_input.value.append( csv.binary() ? f.offset( i ).type : comma::csv::format::fixed_string ); // quick and dirty
            }
        }
        
        if( verbose ) { std::cerr << name() << "csv fields: " << csv.fields << std::endl; }
        
        op_type type = block_indexing;
        default_output = default_input;
        csv.fields = comma::join( v, ',' );
        if( verbose ) { std::cerr << name() << "csv fields: " << csv.fields << std::endl; }
        if( operation == "index" )
        {
            if( !has_block ) { std::cerr << name() << "block field is required for blocking indexing mode" << std::endl; exit(1); }
            csv_out.fields = csv.fields + ',' +  "value/" + default_output.value.append( comma::csv::format::uint32 );
            
            reverse_index = !options.exists( "--no-reverse" );
        }
        else    // operation is head
        {
            type = head_read;
            csv_out = csv;
        }
            if( verbose ) { std::cerr << name() << "out fields: " << csv_out.fields << std::endl; }
        
        comma::csv::input_stream< input_t > istream( std::cin, csv, default_input );
//         comma::csv::output_stream< input_t > ostream( std::cout, csv_out, default_output );
//         if( default_input.key.empty() ) { std::cerr << name() << "please specify at least one id field" << std::endl; return 1; }
        
        
        
        if( !first_line.empty() ) 
        { 
            input_t p = comma::csv::ascii< input_t >( csv, default_input ).get( first_line ); 
            switch( type )
            {
                case head_read:
                    output_from_stdin_head( p );
                default:
                    output_with_appened_index( p );
                break;
            }
        }
        while( !is_shutdown && ( istream.ready() || ( std::cin.good() && !std::cin.eof() ) ) )
        {
            const input_t* p = istream.read();
            if( !p ) 
            { 
                if( type == head_read ) return 2;
                break; 
            }
            
            switch( type )
            {
                case head_read:
                    output_from_stdin_head( *p );
                default:
                    output_with_appened_index( *p );
                break;
            }
        }
        
        switch( type )
        {
            default:
                flush_indexing( block_records );
            break;
        }
        
        
        
        return 0;
    }
    catch( std::exception& ex ) { std::cerr << name() << ex.what() << std::endl; }
    catch( ... ) { std::cerr << name() << "unknown exception" << std::endl; }
    return 1;
}
