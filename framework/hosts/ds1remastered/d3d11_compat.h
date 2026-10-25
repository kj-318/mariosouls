#pragma once

// D3D11 declarations missing from the d3d11.h this toolchain resolves.
//
// After checking each type individually against the bundled header, the only
// genuinely absent one needed by the geometry-pass test draw is ID3D11Blob,
// which appears in the CreateVertexShader / CreatePixelShader signatures as the
// "errors" out-parameter.  Everything else the host needs --
// ID3D11DepthStencilState, D3D11_BLEND_DESC, D3D11_RASTERIZER_DESC,
// D3D11_INPUT_ELEMENT_DESC, D3D11_BUFFER_DESC, D3D11_SUBRESOURCE_DATA,
// D3D11_VIEWPORT -- is defined by the bundled header and is deliberately not
// redefined here.
//
// Wider versions of this file copied D3D11_BLEND_DESC and
// D3D11_DEPTH_STENCIL_DESC out of the Windows SDK and failed to compile with
// "redefinition", because the bundled header already had them; a grep for the
// type name had missed that.  Structs are not hand-declared here: a hand-written
// struct with a wrong field order is exactly the class of bug that produced a
// wrong Format offset earlier in this host.

#include <d3d11.h>

#ifndef MARIOSOULS_D3D11_COMPAT

struct ID3D11Blob;

#define MARIOSOULS_D3D11_COMPAT 1

#endif // MARIOSOULS_D3D11_COMPAT