#include <cassert>

#include <lgr_meshing.hpp>
#include <lgr_macros.hpp>
#include <lgr_state.hpp>
#include <lgr_fill.hpp>
#include <lgr_int_range_product.hpp>
#include <lgr_input.hpp>

namespace lgr {

static void LGR_NOINLINE invert_connectivity(state& s) {
  vector<int, device_allocator<int>, node_index> counts_vector(
      s.nodes.size(), device_allocator<int>(s.mempool));
  lgr::fill(counts_vector, int(0));
  auto const elements_to_element_nodes = s.elements * s.nodes_in_element;
  auto const element_nodes_to_nodes = s.elements_to_nodes.cbegin();
  auto const nodes_to_count = counts_vector.begin();
  auto count_functor = [=](element_index const element) {
    auto const element_nodes = elements_to_element_nodes[element];
    for (auto const element_node : element_nodes) {
      node_index const node = element_nodes_to_nodes[element_node];
      { // needs to be atomic!
      int count = nodes_to_count[node];
      ++count;
      nodes_to_count[node] = count;
      }
    }
  };
  lgr::for_each(s.elements, count_functor);
  s.nodes_to_node_elements.assign_sizes(counts_vector);
  lgr::fill(counts_vector, int(0));
  auto const nodes_to_node_elements = s.nodes_to_node_elements.cbegin();
  auto const node_elements_to_elements = s.node_elements_to_elements.begin();
  auto const node_elements_to_nodes_in_element = s.node_elements_to_nodes_in_element.begin();
  auto fill_functor = [=](element_index const element) {
    auto const element_nodes = elements_to_element_nodes[element];
    for (auto const node_in_element : nodes_in_element) {
      auto const element_node = element_nodes[node_in_element];
      auto const node = element_nodes_to_nodes[element_node];
      int offset;
      { // needs to be atomic!
      offset = nodes_to_count[node];
      nodes_to_count[node] = offset + 1;
      }
      auto const node_elements_range = nodes_to_node_elements[node];
      auto const node_element = node_elements_range[offset];
      node_elements_to_elements_iterator[node_element] = element;
      node_elements_to_nodes_in_element_iterator[node_element] = node_in_element;
    }
  };
  lgr::for_each(s.elements, fill_functor);
}

static void LGR_NOINLINE initialize_bars_to_nodes(state& s) {
  auto const elements_to_element_nodes = s.elements * s.nodes_in_element;
  auto const begin = s.elements_to_nodes.begin();
  auto functor = [=] (element_index const element) {
    auto const element_nodes = elements_to_element_nodes[element];
    using l_t = node_in_element_index;
    begin[element_nodes[l_t(0)]] = node_index(int(element));
    begin[element_nodes[l_t(1)]] = node_index(int(element) + 1);
  };
  lgr::for_each(s.elements, functor);
}

static void LGR_NOINLINE initialize_x_1D(input const& in, state& s) {
  auto const nodes_to_x = s.x.begin();
  auto const num_nodes = s.nodes.size();
  auto const l = in.x_domain_size;
  auto functor = [=](node_index const node) {
    nodes_to_x[node] = vector3<double>(l * (double(int(node)) / double(int(num_nodes) - 1)), 0.0, 0.0);
  };
  lgr::for_each(s.nodes, functor);
}

static void build_bar_mesh(input const& in, state& s) {
  s.elements.resize(element_index(in.elements_along_x));
  s.nodes_in_element.resize(node_in_element_index(2));
  s.nodes.resize(node_index(int(s.elements.size()) + 1));
  s.elements_to_nodes.resize(s.elements.size() * s.nodes_in_element.size());
  initialize_bars_to_nodes(s);
  s.x.resize(s.nodes.size());
  initialize_x_1D(in, s);
}

static void LGR_NOINLINE build_triangle_mesh(input const& in, state& s)
{
  assert(in.elements_along_x >= 1);
  int const nx = in.elements_along_x;
  assert(in.elements_along_y >= 1);
  int const ny = in.elements_along_y;
  s.nodes_in_element.resize(node_in_element_index(3));
  int const nvx = nx + 1;
  int const nvy = ny + 1;
  int const nv = nvx * nvy;
  s.nodes.resize(node_index(nv));
  int const nq = nx * ny;
  int const nt = nq * 2;
  s.elements.resize(element_index(nt));
  s.elements_to_nodes.resize(s.elements.size() * s.nodes_in_element.size());
  auto const element_nodes_to_nodes = s.elements_to_nodes.begin();
  auto const elements_to_element_nodes = s.elements * s.nodes_in_element;
  auto connectivity_functor = [=] (int const quad) {
    int const i = quad % nx;
    int const j = quad / nx;
    auto tri = element_index(quad * 2 + 0);
    auto element_nodes = elements_to_element_nodes[tri];
    using l_t = node_in_element_index;
    using g_t = node_index;
    element_nodes_to_nodes[element_nodes[l_t(0)]] = g_t((j + 0) * nvx + (i + 0));
    element_nodes_to_nodes[element_nodes[l_t(1)]] = g_t((j + 0) * nvx + (i + 1));
    element_nodes_to_nodes[element_nodes[l_t(2)]] = g_t((j + 1) * nvx + (i + 1));
    tri = element_index(quad * 2 + 1);
    element_nodes = elements_to_element_nodes[tri];
    element_nodes_to_nodes[element_nodes[l_t(0)]] = g_t((j + 1) * nvx + (i + 1));
    element_nodes_to_nodes[element_nodes[l_t(1)]] = g_t((j + 1) * nvx + (i + 0));
    element_nodes_to_nodes[element_nodes[l_t(2)]] = g_t((j + 0) * nvx + (i + 0));
  };
  int_range quads(nq);
  lgr::for_each(quads, connectivity_functor);
  s.x.resize(s.nodes.size());
  auto const nodes_to_x = s.x.begin();
  double const x = in.x_domain_size;
  double const y = in.y_domain_size;
  double const dx = x / nx;
  double const dy = y / ny;
  auto coordinates_functor = [=] (node_index const node) {
    int const i = int(node) % nvx;
    int const j = int(node) / nvx;
    nodes_to_x[node] = vector3<double>(i * dx, j * dy, 0.0);
  };
  lgr::for_each(s.nodes, coordinates_functor);
}

static void LGR_NOINLINE build_tetrahedron_mesh(input const& in, state& s)
{
  assert(in.elements_along_x >= 1);
  int const nx = in.elements_along_x;
  assert(in.elements_along_y >= 1);
  int const ny = in.elements_along_y;
  assert(in.elements_along_z >= 1);
  int const nz = in.elements_along_z;
  s.nodes_in_element.resize(node_in_element_index(4));
  int const nvx = nx + 1;
  int const nvy = ny + 1;
  int const nvz = nz + 1;
  int const nvxy = nvx * nvy;
  int const nv = nvxy * nvz;
  s.nodes.resize(node_index(nv));
  int const nxy = nx * ny;
  int const nh = nxy * nz;
  int const nt = nh * 6;
  s.elements.resize(element_index(nt));
  s.elements_to_nodes.resize(s.elements.size() * s.nodes_in_element.size());
  auto const elements_to_nodes = s.elements_to_nodes.begin();
  auto const elements_to_element_nodes = s.elements * s.nodes_in_element;
  auto connectivity_functor = [=] (int const hex) {
    int const ij = hex % nxy;
    int const k = hex / nxy;
    int const i = ij % nx;
    int const j = ij / nx;
    using g_t = node_index;
    node_index hex_nodes[8] = {
      g_t(((k + 0) * nvy + (j + 0)) * nvx + (i + 0)),
      g_t(((k + 0) * nvy + (j + 0)) * nvx + (i + 1)),
      g_t(((k + 0) * nvy + (j + 1)) * nvx + (i + 0)),
      g_t(((k + 0) * nvy + (j + 1)) * nvx + (i + 1)),
      g_t(((k + 1) * nvy + (j + 0)) * nvx + (i + 0)),
      g_t(((k + 1) * nvy + (j + 0)) * nvx + (i + 1)),
      g_t(((k + 1) * nvy + (j + 1)) * nvx + (i + 0)),
      g_t(((k + 1) * nvy + (j + 1)) * nvx + (i + 1))
    };
    auto tet = element_index(hex * 6 + 0);
    auto element_nodes = elements_to_element_nodes[tet];
    using l_t = node_in_element_index;
    elements_to_nodes[element_nodes[l_t(0)]] = hex_nodes[0];
    elements_to_nodes[element_nodes[l_t(1)]] = hex_nodes[3];
    elements_to_nodes[element_nodes[l_t(2)]] = hex_nodes[2];
    elements_to_nodes[element_nodes[l_t(3)]] = hex_nodes[7];
    tet = element_index(hex * 6 + 1);
    element_nodes = elements_to_element_nodes[tet];
    elements_to_nodes[element_nodes[l_t(0)]] = hex_nodes[0];
    elements_to_nodes[element_nodes[l_t(1)]] = hex_nodes[2];
    elements_to_nodes[element_nodes[l_t(2)]] = hex_nodes[6];
    elements_to_nodes[element_nodes[l_t(3)]] = hex_nodes[7];
    tet = element_index(hex * 6 + 2);
    element_nodes = elements_to_element_nodes[tet];
    elements_to_nodes[element_nodes[l_t(0)]] = hex_nodes[0];
    elements_to_nodes[element_nodes[l_t(1)]] = hex_nodes[6];
    elements_to_nodes[element_nodes[l_t(2)]] = hex_nodes[4];
    elements_to_nodes[element_nodes[l_t(3)]] = hex_nodes[7];
    tet = element_index(hex * 6 + 3);
    element_nodes = elements_to_element_nodes[tet];
    elements_to_nodes[element_nodes[l_t(0)]] = hex_nodes[0];
    elements_to_nodes[element_nodes[l_t(1)]] = hex_nodes[5];
    elements_to_nodes[element_nodes[l_t(2)]] = hex_nodes[7];
    elements_to_nodes[element_nodes[l_t(3)]] = hex_nodes[4];
    tet = element_index(hex * 6 + 4);
    element_nodes = elements_to_element_nodes[tet];
    elements_to_nodes[element_nodes[l_t(0)]] = hex_nodes[1];
    elements_to_nodes[element_nodes[l_t(1)]] = hex_nodes[5];
    elements_to_nodes[element_nodes[l_t(2)]] = hex_nodes[7];
    elements_to_nodes[element_nodes[l_t(3)]] = hex_nodes[0];
    tet = element_index(hex * 6 + 5);
    element_nodes = elements_to_element_nodes[tet];
    elements_to_nodes[element_nodes[l_t(0)]] = hex_nodes[1];
    elements_to_nodes[element_nodes[l_t(1)]] = hex_nodes[7];
    elements_to_nodes[element_nodes[l_t(2)]] = hex_nodes[3];
    elements_to_nodes[element_nodes[l_t(3)]] = hex_nodes[0];
  };
  int_range hexes(nh);
  lgr::for_each(hexes, connectivity_functor);
  s.x.resize(s.nodes.size());
  auto const nodes_to_x = s.x.begin();
  double const x = in.x_domain_size;
  double const y = in.y_domain_size;
  double const z = in.z_domain_size;
  double const dx = x / nx;
  double const dy = y / ny;
  double const dz = z / nz;
  auto coordinates_functor = [=] (node_index const node) {
    int const ij = int(node) % nvxy;
    int const k = int(node) / nvxy;
    int const i = ij % nvx;
    int const j = ij / nvx;
    nodes_to_x[node] = vector3<double>(i * dx, j * dy, k * dz);
  };
  lgr::for_each(s.nodes, coordinates_functor);
}

void build_mesh(input const& in, state& s) {
  switch (in.element) {
    case BAR: build_bar_mesh(in, s); break;
    case TRIANGLE: build_triangle_mesh(in, s); break;
    case TETRAHEDRON: build_tetrahedron_mesh(in, s); break;
  }
  s.nodes_to_node_elements.resize(int(s.nodes.size()));
  s.node_elements_to_elements.resize(int(s.elements.size() * s.nodes_in_element.size()));
  s.node_elements_to_nodes_in_element.resize(int(s.elements.size() * s.nodes_in_element.size()));
  invert_connectivity(s.nodes, s.elements, s.nodes_in_element, 
      s.elements_to_nodes, &s.nodes_to_node_elements,
      &s.node_elements_to_elements, &s.node_elements_to_nodes_in_element);
}

}
