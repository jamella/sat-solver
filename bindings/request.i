/*
 * Document-class: Request
 * Request represents a Set of Jobs as input for the Solver.
 *
 */

%{

/*
 * iterating over jobs of a request ('yield' in Ruby)
 */

static int
request_jobs_iterate_callback( const Job *j )
{
#if defined(SWIGRUBY)
  /* FIXME: how to pass 'break' back to the caller ? */
  rb_yield(SWIG_NewPointerObj((void*) j, SWIGTYPE_p__Job, 0));
#endif
  return 0;
}

%}


%nodefault _Request;
%rename(Request) _Request;
typedef struct _Request {} Request;

#if defined(SWIGRUBY)
%mixin Request "Enumerable";
#endif

%extend Request {
  /*
   * Create request based on Pool
   *
   * See also: Pool.create_request
   */
  Request( Pool *pool )
  { return request_new( pool ); }

  ~Request()
  { request_free( $self ); }

  /*
   * Install request
   *
   * Ensure installation of a solvable by either
   * * specifying it directly
   * * specify it by name
   * * specify a required relation
   *
   * Except when specified directly, the solver is free to choose any
   * solvable matching the request (by name, by relation)
   *
   * call-seq:
   *  request.install(solvable) -> void
   *  request.install("kernel") -> void
   *  request.install(relation) -> void
   *
   */
  void install( XSolvable *xs )
  { request_install_xsolvable( $self, xs ); }

  /*
   * Remove request
   *
   * Ensure removal of a solvable by either
   * * specifying it directly
   * * specify it by name
   * * specify a required relation
   *
   * Except when specified directly, the solver is free to choose any
   * solvable matching the request (by name, by relation)
   *
   * call-seq:
   *  request.remove(solvable) -> void
   *  request.remove("kernel") -> void
   *  request.remove(relation) -> void
   *
   */
  void remove( XSolvable *xs )
  { request_remove_xsolvable( $self, xs ); }

  /*
   * Install solvable by name
   */
  void install( const char *name )
  { request_install_name( $self, name ); }

  /*
   * Remove solvable by name
   *
   */
  void remove( const char *name )
  { request_remove_name( $self, name ); }

  /*
   * Install solvable by relation
   *
   */
  void install( const Relation *rel )
  { request_install_relation( $self, rel ); }

  /*
   * Remove solvable by relation
   *
   */
  void remove( const Relation *rel )
  { return request_remove_relation( $self, rel ); }

#if defined(SWIGRUBY)
  %rename("empty?") empty();
  %typemap(out) int empty
    "$result = $1 ? Qtrue : Qfalse;";
#endif
  /*
   * Check if the request has any jobs attached.
   *
   * call-seq:
   *   request.empty? -> bool
   *
   */
  int empty()
  { return ( $self->queue.count == 0 ); }

  /*
   * Return number of jobs of this request
   *
   */
  int size()
  { return request_size( $self ); }

#if defined(SWIGRUBY)
  %rename("clear!") clear();
#endif
  /*
   * Remove all jobs of this request
   *
   * call-seq:
   *   request.clear! -> void
   *
   */
  void clear()
  { queue_empty( &($self->queue) ); }

#if defined(SWIGRUBY)
  %alias get "[]";
#endif
  /*
   * Get job by index
   *
   * The index is just a convenience access method and
   * does NOT imply any preference/ordering of the Jobs.
   *
   * A Request is always considered a set of Jobs.
   *
   * call-seq:
   *  request.get(42) -> Job
   *
   */
  Job *get( unsigned int i )
  { return request_job_get( $self, i ); }

  /*
   * Iterate over each Job of the Request.
   *
   * call-seq:
   *  request.each { |job| ... }
   *
   */
  void each()
  { request_jobs_iterate( $self, request_jobs_iterate_callback ); }
}

