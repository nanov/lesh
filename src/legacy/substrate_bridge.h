#pragma once

#include "substrate/arena.h"
#include "substrate/hybrid_vector.h"
#include "substrate/inline_vector.h"
#include "substrate/traits.h"

// Strangler bridge. This code predates the lesh namespace; the list below is the
// complete inventory of what legacy borrows from the substrate, and it shrinks as
// legacy does. When it is empty, this header is gone.
using lesh::buffer_pool;
using lesh::char_iterable;
using lesh::char_pointer_hash;
using lesh::hybrid_continuous_simple_vector;
using lesh::hybrid_vector;
using lesh::overload;
using lesh::transparent_string_hash;
