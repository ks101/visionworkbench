// __BEGIN_LICENSE__
// Copyright (C) 2006-2011 United States Government as represented by
// the Administrator of the National Aeronautics and Space Administration.
// All Rights Reserved.
// __END_LICENSE__


#ifndef __VW_STEREO_CORRELATOR_H__
#define __VW_STEREO_CORRELATOR_H__

#include <vw/Image/ImageView.h>
#include <vw/Image/ImageViewRef.h>
#include <vw/Image/MaskViews.h>
#include <vw/Image/Transform.h>
#include <vw/Image/Filter.h>
#include <vw/Stereo/DisparityMap.h>
#include <vw/Stereo/OptimizedCorrelator.h>

namespace vw {
namespace stereo {
  class PyramidCorrelator {

    BBox2f m_initial_search_range;
    Vector2i m_kernel_size;
    float m_cross_correlation_threshold;
    int32 m_cost_blur;
    stereo::CorrelatorType m_correlator_type;
    size_t m_pyramid_levels;
    int32 m_min_subregion_dim;
    typedef PixelMask<Vector2f> PixelDisp;

    std::string m_debug_prefix;

    struct SubsampleMaskByTwoFunc : public ReturnFixedType<uint8> {
      BBox2i work_area() const { return BBox2i(0,0,2,2); }

      template <class PixelAccessorT>
      typename boost::remove_reference<typename PixelAccessorT::pixel_type>::type
      operator()( PixelAccessorT acc ) const {

        typedef typename PixelAccessorT::pixel_type PixelT;

        uint8 count = 0;
        if ( *acc ) count++;
        acc.next_col();
        if ( *acc ) count++;
        acc.advance(-1,1);
        if ( *acc ) count++;
        acc.next_col();
        if ( *acc ) count++;
        if ( count > 1 )
          return PixelT(ScalarTypeLimits<PixelT>::highest());
        return PixelT();
      }
    };

    template <class ViewT>
    SubsampleView<UnaryPerPixelAccessorView<EdgeExtensionView<ViewT,ZeroEdgeExtension>, SubsampleMaskByTwoFunc> >
    subsample_mask_by_two( ImageViewBase<ViewT> const& input ) {
      return subsample(per_pixel_accessor_filter(input.impl(), SubsampleMaskByTwoFunc()),2);
    }

    // Iterate over the nominal blocks, creating output blocks for correlation
    BBox2f compute_matching_blocks(BBox2i const& nominal_block,
                                   BBox2f const& search_range,
                                   BBox2i &left_block, BBox2i &right_block);

    std::vector<BBox2f>
    compute_search_ranges(ImageView<PixelDisp> const& prev_disparity_map,
                          std::vector<BBox2i> const& nominal_blocks);

    void write_debug_images(int32 n, ImageViewRef<PixelDisp> const& disparity_map,
                            std::vector<BBox2i> const& nominal_blocks);
    std::vector<BBox2i>
    subdivide_bboxes(ImageView<PixelDisp> const& disparity_map,
                     ImageView<PixelMask<uint8> > const& valid_pad,
                     BBox2i const& box);

    template <class ViewT>
    size_t count_valid_pixels(ImageViewBase<ViewT> const& img) {
      typedef typename ViewT::const_iterator view_iter;

      size_t count = 0;
      for (view_iter i = img.impl().begin(); i != img.impl().end(); i++) {
        if (i->valid())
          count++;
      }

      return count;
    }

    // do_correlation()
    //
    // Takes an image pyramid of images and conducts dense stereo
    // matching using a pyramid based approach.
    template <class ChannelT, class PreProcFilterT>
    ImageView<PixelDisp>
    do_correlation(std::vector<ImageView<ChannelT> > const& left_pyramid,
                   std::vector<ImageView<ChannelT> > const& right_pyramid,
                   std::vector<ImageView<uint8> > const& left_masks,
                   std::vector<ImageView<uint8> > const& right_masks,
                   PreProcFilterT const& preproc_filter) {

      std::vector<uint8> x_kern(m_kernel_size.x()),
        y_kern(m_kernel_size.y());
      std::fill(x_kern.begin(), x_kern.end(), 1);
      std::fill(y_kern.begin(), y_kern.end(), 1);

      BBox2f initial_search_range =
        m_initial_search_range / pow(2.0f, m_pyramid_levels-1);
      ImageView<PixelDisp > disparity_map;

      // Overall Progress Bar
      TerminalProgressCallback prog( "stereo", "Pyr Search:", DebugMessage);

      // Refined the disparity map by searching in the local region
      // where the last good disparity value was found.
      for (ssize_t n = ssize_t(m_pyramid_levels) - 1; n >=0; --n) {
        std::ostringstream current_level;
        current_level << n;

        SubProgressCallback subbar(prog,float(m_pyramid_levels-1-n)/float(m_pyramid_levels), float(m_pyramid_levels-n)/float(m_pyramid_levels));

        ImageView<PixelDisp> new_disparity_map(left_pyramid[n].cols(),
                                               left_pyramid[n].rows());

        // 1. Subdivide disparity map into subregions.  We build up
        //    the disparity map for the level, one subregion at a
        //    time.  For now, subregions that are 512x512 pixels seems
        //    to be an efficient size.
        //
        //    We also build a list of search ranges from the previous
        //    level's disparity map.  If this is the first level of the
        //    pyramid, we go with the full search range.
        std::vector<BBox2f> search_ranges;
        std::vector<BBox2i> nominal_blocks;
        if (n == (ssize_t(m_pyramid_levels)-1) ) {
          nominal_blocks.push_back(BBox2i(0,0,left_pyramid[n].cols(),
                                          left_pyramid[n].rows()));
          search_ranges.push_back(initial_search_range);
        } else {
          // valid_pad masks all the pixels already masked by
          // disparity_map, with the addition of a m_kernel_size/2 pad
          // around each pixel.  This is used to prevent
          // subdivide_bboxes from rejecting subregions that may
          // actually later get filled with valid pixels at a higher
          // scale (which helps prevent 'cutting' into the disparity map)
          ImageView<PixelMask<uint8> > valid_pad =
            create_mask(separable_convolution_filter(apply_mask(copy_mask(constant_view<uint8>(1, disparity_map.cols(), disparity_map.rows()), disparity_map)), x_kern, y_kern));

          nominal_blocks = subdivide_bboxes(disparity_map, valid_pad,
                                            BBox2i(0,0,left_pyramid[n].cols(),
                                                   left_pyramid[n].rows()));
          search_ranges = compute_search_ranges(disparity_map, nominal_blocks);
        }

        for (size_t r = 0; r < nominal_blocks.size(); ++r) {
          subbar.report_progress((float)r/nominal_blocks.size());

          // Given a block from the left image, compute the bounding
          // box of pixels we will be searching in the right image
          // given the disparity range for the current left image
          // bbox.
          //
          // There's no point in correlating in areas where the second
          // image has no data, so we adjust the block sizes here to avoid
          // doing unnecessary work.
          BBox2i left_block, right_block;
          BBox2i right_image_workarea =
            BBox2i(Vector2i(nominal_blocks[r].min().x()+int(floor(search_ranges[r].min().x())),
                            nominal_blocks[r].min().y()+int(floor(search_ranges[r].min().y()))),
                   Vector2i(nominal_blocks[r].max().x()+int(ceil(search_ranges[r].max().x())),
                            nominal_blocks[r].max().y()+int(ceil(search_ranges[r].max().y()))));
          BBox2i right_image_bounds =
            BBox2i(0,0, right_pyramid[n].cols(), right_pyramid[n].rows());
          right_image_workarea.crop(right_image_bounds);
          if (right_image_workarea.width() == 0 ||
              right_image_workarea.height() == 0) { continue; }
          BBox2f adjusted_search_range =
            compute_matching_blocks(nominal_blocks[r],
                                    search_ranges[r], left_block, right_block);

          //   2. Run the correlation for this level.  We pass in the
          //      offset (difference) between the adjusted_search_range
          //      and original search_ranges[r] so that this can be added
          //      back in when setting the final disparity.
          float h_disp_offset =
            search_ranges[r].min().x() - adjusted_search_range.min().x();
          float v_disp_offset =
            search_ranges[r].min().y() - adjusted_search_range.min().y();

          // Place this block in the proper place in the complete
          // disparity map.
          ImageView<PixelDisp> disparity_block =
            correlate( crop(edge_extend(left_pyramid[n],ReflectEdgeExtension()),
                            left_block),
                       crop(edge_extend(right_pyramid[n],ReflectEdgeExtension()),
                            right_block),
                       adjusted_search_range,
                       Vector2f(h_disp_offset, v_disp_offset),
                       preproc_filter );

          crop(new_disparity_map, nominal_blocks[r]) =
            crop(disparity_block, m_kernel_size[0], m_kernel_size[1],
                 nominal_blocks[r].width(), nominal_blocks[r].height());
        }
        subbar.report_finished();


        // Clean up the disparity map by rejecting outliers in the lower
        // resolution levels of the pyramid.  These are some settings that
        // seem to work well in practice.
        int32 rm_half_kernel = 5;
        float rm_min_matches_percent = 0.5;
        float rm_threshold = 3.0;

        ImageView<PixelDisp> disparity_map_clean =
          disparity_mask(disparity_clean_up(new_disparity_map,
                                            rm_half_kernel, rm_half_kernel,
                                            rm_threshold,
                                            rm_min_matches_percent),
                         left_masks[n], right_masks[n]);

        if (n == ssize_t(m_pyramid_levels) - 1 || n == 0) {
          // At the highest level of the pyramid, use the cleaned version
          // of the disparity map just obtained (since there are no
          // previous results to learn from)
          // At the last level, return the raw results from the correlator
          disparity_map = disparity_map_clean;
        } else {
          // If we have a missing pixel that correlated properly in
          // the previous pyramid level, use the disparity found at
          // the previous pyramid level
          ImageView<PixelDisp > disparity_map_old =
            crop(edge_extend(disparity_upsample(disparity_map),
                             ZeroEdgeExtension()),
                 BBox2i(0, 0, disparity_map_clean.cols(),
                        disparity_map_clean.rows()));
          ImageView<PixelDisp > disparity_map_old_diff =
            invert_mask(intersect_mask(disparity_map_old, disparity_map_clean));
          disparity_map =
            disparity_mask(create_mask(apply_mask(disparity_map_clean) +
                                       apply_mask(disparity_map_old_diff)),
                           left_masks[n], right_masks[n]);
        }

        // Debugging output at each level
        if (!m_debug_prefix.empty())
          write_debug_images(n, disparity_map, nominal_blocks);
      }
      prog.report_finished();

      return disparity_map;
    }

    template <class View1T, class View2T, class PreProcFilterT>
    ImageView<PixelDisp > correlate(ImageViewBase<View1T> const& left_image,
                                    ImageViewBase<View2T> const& right_image,
                                    BBox2f const& search_range,
                                    Vector2f const& offset,
                                    PreProcFilterT const& preproc_filter) {

      stereo::OptimizedCorrelator correlator( BBox2i(Vector2i(int(floor(search_range.min().x())), int(ceil(search_range.min().y()))),
                                                     Vector2i(int(floor(search_range.max().x())), int(ceil(search_range.max().y()))) ),
                                              m_kernel_size[0],
                                              m_cross_correlation_threshold,
                                              m_cost_blur, m_correlator_type);
      return correlator( left_image.impl(),
                         right_image.impl(),
                         preproc_filter ) + PixelDisp(offset);
    }

  public:

    /// Correlator Constructor
    ///
    /// Set pyramid_levels to 0 to force the use of a single pyramid
    /// level (essentially disabling pyramid correlation).
    PyramidCorrelator(BBox2f const& initial_search_range,
                      Vector2i const& kernel_size,
                      float cross_correlation_threshold = 1,
                      int32 cost_blur = 1,
                      stereo::CorrelatorType correlator_type = ABS_DIFF_CORRELATOR,
                      size_t pyramid_levels = 4) :
      m_initial_search_range(initial_search_range),
      m_kernel_size(kernel_size),
      m_cross_correlation_threshold(cross_correlation_threshold),
      m_cost_blur(cost_blur),
      m_correlator_type(correlator_type),
      m_pyramid_levels(pyramid_levels) {
      m_debug_prefix = "";
      m_min_subregion_dim = 128;
    }

    /// Turn on debugging output.  The debug_file_prefix string is
    /// used as a prefix for all debug image files.
    void set_debug_mode(std::string const& debug_file_prefix) { m_debug_prefix = debug_file_prefix; }

    template <class View1T, class View2T, class Mask1T, class Mask2T, class PreProcFilterT>
    ImageView<PixelDisp > operator() (ImageViewBase<View1T> const& left_image,
                                      ImageViewBase<View2T> const& right_image,
                                      ImageViewBase<Mask1T> const& left_mask,
                                      ImageViewBase<Mask2T> const& right_mask,
                                      PreProcFilterT const& preproc_filter) {

      typedef typename View1T::pixel_type pixel_type;
      typedef typename PixelChannelType<pixel_type>::type channel_type;

      VW_ASSERT(left_image.impl().cols() == right_image.impl().cols() &&
                left_image.impl().rows() == right_image.impl().rows(),
                ArgumentErr() << "Correlator(): input image dimensions do not match.");

      VW_ASSERT(left_image.impl().cols() == left_mask.impl().cols() &&
                left_image.impl().rows() == left_mask.impl().rows(),
                ArgumentErr() << "Correlator(): input image and mask dimensions do not match.");

      VW_ASSERT(left_image.impl().cols() == right_mask.impl().cols() &&
                left_image.impl().rows() == right_mask.impl().rows(),
                ArgumentErr() << "Correlator(): input image and mask dimensions do not match.");

      // Compute the number of pyramid levels needed to reach the
      // minimum image resolution set by the user.
      //
      // Level 0 : finest (high resolution) image
      // Level n : coarsest (low resolution) image ( n == pyramid_levels - 1 )
      vw_out(DebugMessage, "stereo") << "Initializing pyramid correlator with "
                                     << m_pyramid_levels << " levels.\n";

      // Build the image pyramid
      std::vector<ImageView<channel_type> > left_pyramid(m_pyramid_levels), right_pyramid(m_pyramid_levels);
      std::vector<ImageView<uint8> > left_masks(m_pyramid_levels), right_masks(m_pyramid_levels);

      // Render the lowest level
      left_pyramid[0] =  pixel_cast<channel_type>(left_image);
      right_pyramid[0] = pixel_cast<channel_type>(right_image);
      left_masks[0] =    pixel_cast<uint8>(left_mask);
      right_masks[0] =   pixel_cast<uint8>(right_mask);

      // Calculate local mean of input images
      channel_type left_mean =
        mean_pixel_value(subsample(copy_mask(left_pyramid[0],create_mask(left_masks[0],0)),2));
      channel_type right_mean =
        mean_pixel_value(subsample(copy_mask(right_pyramid[0],create_mask(right_masks[0],0)),2));

      // Write mean color to nodata of input images
      left_pyramid[0] = apply_mask(copy_mask(left_pyramid[0],create_mask(left_masks[0],0)), left_mean);
      right_pyramid[0] = apply_mask(copy_mask(right_pyramid[0],create_mask(right_masks[0],0)), right_mean);

      // Produce the image pyramid
      for (size_t n = 1; n < m_pyramid_levels; ++n) {
        left_pyramid[n] =  subsample(gaussian_filter(left_pyramid[n-1],1.2),2);
        right_pyramid[n] = subsample(gaussian_filter(right_pyramid[n-1],1.2),2);
        left_masks[n] = subsample_mask_by_two(left_masks[n-1]);
        right_masks[n] = subsample_mask_by_two(right_masks[n-1]);
        left_masks[n] = crop(edge_extend(left_masks[n]),
                             bounding_box(left_pyramid[n]));
        right_masks[n] = crop(edge_extend(right_masks[n]),
                              bounding_box(right_pyramid[n]));
      }

      return do_correlation(left_pyramid, right_pyramid,
                            left_masks, right_masks, preproc_filter);
    }

  };

}} // namespace vw::stereo

#endif // __VW_STEREO_CORRELATOR_H__
