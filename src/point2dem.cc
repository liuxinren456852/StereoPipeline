#ifdef _MSC_VER
#pragma warning(disable:4244)
#pragma warning(disable:4267)
#pragma warning(disable:4996)
#endif

#ifdef NDEBUG
#undef NDEBUG
#endif

#include <stdlib.h>

#include <boost/program_options.hpp>
namespace po = boost::program_options;

#include <vw/FileIO.h>
#include <vw/Image.h>
#include <vw/Stereo.h>
#include <vw/Cartography.h>
using namespace vw;
using namespace vw::stereo;
using namespace vw::cartography;

#include "stereo.h"
#include "DEM.h"
#include "nff_terrain.h"
#include "OrthoRasterizer.h"


// Allows FileIO to correctly read/write these pixel types
namespace vw {
  template<> struct PixelFormatID<Vector3>   { static const PixelFormatEnum value = VW_PIXEL_XYZ; };
}

int main( int argc, char *argv[] ) {
  set_debug_level(VerboseDebugMessage+11);

  std::string input_file_name, out_prefix, output_file_type, texture_filename;
  unsigned cache_size, max_triangles;
  float dem_spacing, default_value;
  unsigned simplemesh_h_step, simplemesh_v_step;
  int debug_level;
  double z_reference;
  std::string reference_spheroid;

  po::options_description desc("Options");
  desc.add_options()
    ("help", "Display this help message")
    ("minz-is-default", "Use the smallest z value as the default (missing pixel) value")
    ("default_value", po::value<float>(&default_value)->default_value(0), "Explicitly set the default (missing pixel) value")
    ("dem-spacing,s", po::value<float>(&dem_spacing)->default_value(0), "Set the DEM post size (if this value is 0, the post spacing size is computed for you)")
    ("normalized,n", "Also write a normalized version of the DEM (for debugging)")    
    ("orthoimage", po::value<std::string>(&texture_filename), "Write an orthoimage based on the texture file given as an argument to this command line option")
    ("grayscale", "Use grayscale image processing for creating the orthoimage")
    ("offset-files", "Also write a pair of ascii offset files (for debugging)")    
    ("cache", po::value<unsigned>(&cache_size)->default_value(1024), "Cache size, in megabytes")
    ("input-file", po::value<std::string>(&input_file_name), "Explicitly specify the input file")
    ("texture-file", po::value<std::string>(&texture_filename), "Specify texture filename")
    ("output-prefix,o", po::value<std::string>(&out_prefix)->default_value("terrain"), "Specify the output prefix")
    ("output-filetype,t", po::value<std::string>(&output_file_type)->default_value("tif"), "Specify the output file")
    ("debug-level,d", po::value<int>(&debug_level)->default_value(vw::DebugMessage-1), "Set the debugging output level. (0-50+)")
    ("xyz-to-lonlat", "Convert from xyz coordinates to longitude, latitude, altitude coordinates.")
    ("z-reference", po::value<double>(&z_reference)->default_value(0),"Set a reference Z value.  This will be subtracted from all z values.")
    ("reference-spheroid,r", po::value<std::string>(&reference_spheroid)->default_value(""),"Set a reference surface.  The radius of the reference surface will be subtracted from all z values.");
  
  po::positional_options_description p;
  p.add("input-file", 1);

  po::variables_map vm;
  po::store( po::command_line_parser( argc, argv ).options(desc).positional(p).run(), vm );
  po::notify( vm );

  // Set the Vision Workbench debug level
  set_debug_level(debug_level);
  Cache::system_cache().resize( cache_size*1024*1024 ); 

  if( vm.count("help") ) {
    std::cout << desc << std::endl;
    return 1;
  }

  if( vm.count("input-file") != 1 ) {
    std::cout << "Error: Must specify exactly one pointcloud file and one texture file!" << std::endl;
    std::cout << desc << std::endl;
    return 1;
  }

  DiskImageView<Vector3> point_disk_image(input_file_name);
  ImageViewRef<Vector3> point_image = point_disk_image;
  
  if (vm.count("xyz-to-lonlat") ) {
    std::cout << "Reprojecting points into longitude, latitude, altitude.\n";
    point_image = cartography::xyz_to_lon_lat_radius(point_image);
  }

  if ( reference_spheroid != "" ) {
    if (reference_spheroid == "mars") {
      const double MOLA_PEDR_EQUATORIAL_RADIUS = 3396000.0;
      std::cout << "Re-referencing altitude values using standard MOLA spherical radius: " << MOLA_PEDR_EQUATORIAL_RADIUS << "\n";
      cartography::Datum datum = cartography::Datum("Mars Datum",
                                                    "MOLA PEDR Spheroid",
                                                    "Prime Meridian",
                                                    MOLA_PEDR_EQUATORIAL_RADIUS,
                                                    MOLA_PEDR_EQUATORIAL_RADIUS,
                                                    0.0);
      point_image = cartography::subtract_datum(point_image, datum);
    } else {
      std::cout << "Unknown reference spheroid: " << reference_spheroid << ".  Exiting.\n\n";
      exit(0);
    }
  } else if (vm.count("z_reference") ) {
    std::cout << "Re-referencing altitude values using user supplied z offset: " << z_reference << "\n";
    cartography::Datum datum = cartography::Datum("User Specified Datum",
                                                  "User Specified Spherem",
                                                  "Prime Meridian",
                                                  z_reference, z_reference, 0.0);
    point_image = cartography::subtract_datum(point_image, datum);
  }

  DiskCacheImageView<Vector3> point_image_cache(point_image, "exr");

  // Write out the DEM, texture, and extrapolation mask
  // as georeferenced files.
  vw::cartography::OrthoRasterizerView<PixelGray<float> > rasterizer(point_image_cache, select_channel(point_image_cache,2),dem_spacing);
  if (vm.count("minz-is-default") )
    rasterizer.set_use_minz_as_default(true); 
  else       
    rasterizer.set_use_minz_as_default(false); 
  rasterizer.set_default_value(default_value);    
  vw::BBox<float,3> dem_bbox = rasterizer.bounding_box();
  std::cout << "DEM Bounding box: " << dem_bbox << "\n";
  
  // Set up the georeferencing information
  // FIXME: Using Mercator projection for now 
  GeoReference georef;
  georef.set_mercator(0,0,1);
  georef.set_transform(rasterizer.geo_transform());

    // Write out a georeferenced orthoimage of the DTM with alpha.
  if (vm.count("orthoimage")) {
    DiskImageView<PixelRGB<float> > texture(texture_filename); 
    rasterizer.set_texture(texture);
    BlockCacheView<PixelRGB<float> > block_drg_raster(rasterizer, Vector2i(rasterizer.cols(), 512));
    write_georeferenced_image(out_prefix + "-DRG.tif", channel_cast_rescale<uint8>(block_drg_raster), georef, TerminalProgressCallback() );
  } else {
    BlockCacheView<PixelGray<float> > block_dem_raster(rasterizer, Vector2i(rasterizer.cols(), 512));
    write_georeferenced_image(out_prefix + "-DEM." + output_file_type, block_dem_raster, georef, TerminalProgressCallback());
    
    if (vm.count("normalized")) {
      DiskImageView<PixelGray<float> > dem_image(out_prefix + "-DEM." + output_file_type);
      write_georeferenced_image(out_prefix + "-DEM-normalized.tif", channel_cast_rescale<uint8>(normalize(dem_image)), georef, TerminalProgressCallback());
    }
  }

  if (vm.count("offset-files")) {
    // Write out the offset files
    std::cout << "Offset: " << dem_bbox.min().x()/rasterizer.spacing() << "   " << dem_bbox.max().y()/rasterizer.spacing() << "\n";
    std::string offset_filename = out_prefix + "-DRG.offset";
    FILE* offset_file = fopen(offset_filename.c_str(), "w");
    fprintf(offset_file, "%d\n%d\n", int(dem_bbox.min().x()/rasterizer.spacing()), -int(dem_bbox.max().y()/rasterizer.spacing()));
    fclose(offset_file);
    offset_filename = out_prefix + "-DEM-normalized.offset";
    offset_file = fopen(offset_filename.c_str(), "w");
    fprintf(offset_file, "%d\n%d\n", int(dem_bbox.min().x()/rasterizer.spacing()), -int(dem_bbox.max().y()/rasterizer.spacing()));
    fclose(offset_file);
  }
  return 0;
}