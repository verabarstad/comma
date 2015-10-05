import numpy
import sys
from StringIO import StringIO
import itertools
import io
import warnings
import operator
import re
import comma.csv.types

def shape_unrolled_types_of_flat_dtype( dtype ):
  shape_unrolled_types = []
  for descr in dtype.descr:
    type = descr[1]
    shape = descr[2] if len( descr ) > 2 else ()
    shape_unrolled_types.extend( [ type ] * reduce( operator.mul, shape, 1 ) )
  return tuple( shape_unrolled_types )
  
class struct:
  def __init__( self, concise_fields, *concise_types ):
    if len( concise_fields.split(',') ) != len( concise_types ):
      raise Exception( "expected {} types for '{}', got {} type(s)".format( len( concise_fields.split(',') ), concise_fields, len( concise_types )) )
    self.dtype = numpy.dtype( zip( concise_fields.split(','), concise_types ) )
    fields, types = [], []
    for name, type in zip( concise_fields.split(','), concise_types ):
      if isinstance( type, struct ):
        fields.extend( map( lambda _: name + '/' + _, type.fields ) )
        types.extend( type.types )
      else:
        fields.append( name )
        types.append( type )    
    self.fields = tuple( fields )
    self.types = tuple( types )
    self.flat_dtype = numpy.dtype( zip( self.fields, self.types ) )
    format = ','.join( shape_unrolled_types_of_flat_dtype( self.flat_dtype ) )
    number_of_types = len( re.sub( r'\(.+\)', '', format ).split(',') )
    self.unrolled_flat_dtype = numpy.dtype( [ ( 'f0', format ) ] ) if number_of_types == 1 else numpy.dtype( format )

class stream:
  buffer_size_in_bytes = 65536
  def __init__( self, s, fields=None, format=None, binary=False, delimiter=',', flush=False ):
    if not isinstance( s, struct ): raise Exception( "expected '{}', got '{}'".format( str( struct ), repr( s ) ) )
    self.struct = s
    if fields:
      if not set( fields.split(',') ).issuperset( self.struct.fields ):
        unaccounted_fields = set( self.struct.fields ) - set( fields.split(',') )
        raise Exception( "expected field(s) '{}' not found in supplied fields '{}'".format( ','.join( unaccounted_fields ), fields ) )
      self.fields = tuple( fields.split(',') )
    else:
      self.fields = self.struct.fields
    self.binary = binary or format is not None
    self.delimiter = delimiter if not self.binary else None
    self.flush = flush
    if format:
      self.dtype = numpy.dtype( format )
    else:
      if self.fields == self.struct.fields:
        self.dtype = self.struct.flat_dtype
      else:
        struct_type_of_field = dict( zip( self.struct.fields, self.struct.types ) )
        names = [ 'f' + str( i ) for i in range( len( self.fields ) ) ]
        types_ = [ struct_type_of_field.get( name ) or 'S' for name in self.fields ]
        self.dtype = numpy.dtype( zip( names, types_ ) )
    if format and len( self.fields ) != len( self.dtype.names ):
      raise Exception( "expected same number of fields and format types, got '{}' and '{}'".format( self.fields, format ) )    
    self.size = max( 1, stream.buffer_size_in_bytes / self.dtype.itemsize )
    if not self.binary:
      self.converters = types.ascii_time_converters( shape_unrolled_types_of_flat_dtype( self.dtype ) )
    if self.fields == self.struct.fields:
        self.reshaped_dtype = None
    else:
      names = [ 'f' + str( self.fields.index( name ) ) for name in self.struct.fields ]
      formats = [ self.dtype.fields[name][0] for name in names ]
      offsets = [ self.dtype.fields[name][1] for name in names ]
      self.reshaped_dtype = numpy.dtype( dict( names=names, formats=formats, offsets=offsets ) )

  def iter( self, size=None, recarray=False  ):
    size = self.size if size is None else size
    while True:
      s = self.read( size, recarray )
      if s is None: break
      yield s

  def read( self, size, recarray=False ):
    if self.binary:
      data = numpy.fromfile( sys.stdin, dtype=self.dtype, count=size )
    else:
      with warnings.catch_warnings():
        warnings.simplefilter( 'ignore' )
        data = numpy.loadtxt( StringIO( ''.join( itertools.islice( sys.stdin, size ) ) ), dtype=self.dtype , delimiter=self.delimiter, converters=self.converters, ndmin=1 )
    if data.size == 0: return None
    if self.reshaped_dtype:
      s = numpy.array( map( tuple, numpy.ndarray( data.shape, self.reshaped_dtype, data, strides=data.itemsize ) ), dtype=self.struct.flat_dtype ).view( self.struct )
    else:
      s = data.view( self.struct )
    return s.view( numpy.recarray ) if recarray else s

  def write( self, s, flush=None ):
    if self.binary:
      s.tofile( sys.stdout )
    else:
      to_string = lambda _: types.time_from_numpy( _ ) if isinstance( _, numpy.datetime64 ) else str( _ )
      for _ in s.view( self.struct.unrolled_flat_dtype ):
        print self.delimiter.join( map( to_string, _ ) )
    flush = self.flush if flush is None else flush
    if flush: sys.stdout.flush()
