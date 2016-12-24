#include "venus/blend.h"
#include "venus/compiler.h"
#include "venus/Feature.h"
#include "venus/ImageWarp.h"
#include "venus/Makeup.h"
#include "venus/opencv_utility.h"
#include "venus/Scalar.h"

#include <opencv2/imgproc.hpp>

// OpenCV's inpaint algorithm is lame, needs an alternative.
#define USE_OPENCV_INPAINT 0
#if USE_OPENCV_INPAINT
#  include <opencv2/photo.hpp>
#else
#  include "venus/inpaint.h"
#endif

using namespace cv;

namespace venus {

cv::Mat Makeup::pack(const cv::Mat& mask, uint32_t color)
{
	assert(mask.type() == CV_8UC1);
	cv::Mat image(mask.rows, mask.cols, CV_8UC4);

	const int length = mask.rows * mask.cols;
	const uint8_t* mask_data = mask.data;
	uint32_t* image_data = reinterpret_cast<uint32_t*>(image.data);

	#pragma omp parallel for
	for(int i = 0; i < length; ++i)
	{
		uint8_t alpha = ((color >> 24) * mask_data[i] + 127) / 255;
#if USE_BGRA_LAYOUT
		// Swap R and B channel, then assembly it to BGRA format.
		image_data[i] = ((color >> 16) & 0xFF) | (color &0x00FF00) | ((color & 0xFF) << 16) | (alpha << 24);
#else
		image_data[i] = (color & 0x00FFFFFF) | (alpha << 24);
#endif
	}

	return image;
}

std::vector<cv::Point2f> Makeup::createHeartShape(const cv::Point2f& center, float radius, float angle/* = 0.0F*/)
{
	const int N = 32;  // can be tweaked!
	std::vector<Point2f> heart(N);

	const float cosa = std::cos(angle), sina = std::sin(angle);

	// http://mathworld.wolfram.com/HeartCurve.html
	// x = 16*sin(t)^3
	// y = 13*cos(t) - 5*cos(2*t) - 2*cos(3*t) - cos(4*t)
	// where parameter t is in range [0 : 2*pi]
	//
	// cos(2*t) = cos(t)^2 - sin(t)^2
	// cos(3*t) = 4*cos(t)^3 - 3*cos(t)
	// cos(4*t) = cos(2*t)^2 - sin(2*t)^2
	for(int i = 0; i < N; ++i)
	{
		float t     = i * static_cast<float>(2*M_PI / N);
		float sint  = std::sin(t), cost  = std::cos(t);
		float sin2t = 2*sint*cost, cos2t = cost*cost - sint*sint;
		float cos3t = cost *cos2t - sint *sin2t;
		float cos4t = cos2t*cos2t - sin2t*sin2t;

		float x = sint * sint * sint;
		// A negative sign makes Y up coordinates to Y down coordinates.
		float y = (13*cost - 5*cos2t - 2*cos3t - cos4t) / -16;

		// rotate (x, y) by angle:
		// (x + y*i)*(cosa + sina * i) = (x*cosa - y*sina) + (x*sina + y*cosa)*i
		Point2f rotated(x*cosa - y*sina, x*sina + y*cosa);
		heart[i] = center + radius * rotated;
	}
	return heart;
}

std::vector<cv::Point2f> Makeup::createPolygon(const std::vector<cv::Point2f>& points, BlushShape shape, bool right)
{
	assert(points.size() == Feature::COUNT);
	
	const Point2f& _02 = points[right ?  2:10];
	const Point2f& _62 = points[right ? 62:58];
	const Point2f& _00 = points[right ?  0:12];
	const Point2f& _01 = points[right ?  1:11];
	const Point2f& _33 = points[right ? 33:32];
	const Point2f& _41 = points[right ? 41:51];
	const Point2f& _61 = points[right ? 61:59];
	const Point2f& _03 = points[right ?  3: 9];

	switch(shape)
	{
	case BlushShape::DEFAULT:
		return Feature::calculateBlushPolygon(points, right);
		break;

	case BlushShape::DISK:
	{
		Point2f center((_62.x + _02.x)/2, _62.y);
		float radius = std::abs(_62.x - _02.x)/2;
		const int N = 12;  // can be tuned
		std::vector<Point2f> circle(N);
		for(int i = 0; i < N; ++i)
		{
			float t = i/static_cast<float>(2 * M_PI) * N;
			circle[i] = center + radius * Point2f(std::cos(t), std::sin(t));
		}
		
		return circle;
	}
		break;

	case BlushShape::OVAL:
		return std::vector<Point2f>
		{
			(_00 + _01*2)/3,
			_01,
			(_01*2 + _02)/3,
			(_01 + _02*2)/3,
			Point2f(_33.x, _61.y),
			_62,
			Point2f(_41.x, points[53].y)
		};
		break;

	case BlushShape::TRIANGLE:
	{
		Point2f down = points[56] - points[53];
		down /= (down.x*down.x + down.y*down.y);
		return std::vector<Point2f>
		{
			Point2f(_33.x, _62.y),
			(_02 + _03)/2,
			_02,
			catmullRomSpline(2.0f/3, _00, _01, _02, _03),
			catmullRomSpline(1.0f/3, _00, _01, _02, _03),
			_01,
			(_00 + _01*2)/3,
		};
	}
		break;

	case BlushShape::HEART:
	{
		Point2f _x = (_62 + _02)/2;
		Point2f _y = (points[53] + points[56]*2)/3;

		Vec4f line = Feature::getSymmetryAxis(points);
		float radius = std::abs(distance(_62, line) - distance(_02, line));
		
		Point2f down(line[0], line[1]);
		float d = distance(_x, line);
		Point2f N = right?Point2f(line[1], -line[0]):Point2f(-line[1], line[0]);
		Point2f center = _y + d * N;
		
		float angle = std::atan2(down.y, down.x) - static_cast<float>(M_PI/2);

		return createHeartShape(center, radius, angle);
	}
		break;

	case BlushShape::SEAGULL:
	{
		const uint8_t knot_r[5] = { 42, 22, 23, 24, 25 };
		const uint8_t knot_l[5] = { 43, 29, 30, 31, 26 };
		const uint8_t* knot = right? knot_r:knot_l;
		
		// Feature::getSymmetryAxis() is expensive
		Point2f down = points[56] - points[53];
		down /= std::sqrt(down.x*down.x + down.y*down.y);

		const int N = 10;
		std::vector<Point2f> seagull(N);
		seagull[0] = _01;
		seagull[5] = points[right?54:52];

		const Point2f& carriage = points[knot[0]];
		for(int i = 1; i < 5; ++i)
		{
			const Point2f& point = points[knot[i]];
			Point2f d = carriage - point;
			float dot = d.x * down.x + d.y * down.y;  // project on vector down
			seagull[i]    = point + 3*(dot * down);
			seagull[10-i] = point + 2*(dot * down);
		}
		
		return seagull;
	}
		break;

	default:
		assert(false);
		return std::vector<cv::Point2f>();
	}
}

void Makeup::blend(cv::Mat& result, const cv::Mat& dst, const cv::Mat& src, const cv::Point2i& origin, float amount)
{
	assert(!src.empty() && (src.type() == CV_8UC4 || src.type() == CV_32FC4));

	// Note that dst.copyTo(result); will invoke result.create(src.size(), src.type());
	// which has this clause if( dims <= 2 && rows == _rows && cols == _cols && type() == _type && data ) return;
	// which means that result's memory will only be allocated the first time in if result is empty.
	if(result.data != dst.data)
		dst.copyTo(result);

	Rect rect_src(origin.x, origin.y, src.cols, src.rows);
	Rect rect_dst(0, 0, dst.cols, dst.rows);
	Rect rect = rect_dst & rect_src;

	switch(dst.type())
	{
#if 0  // currently unused case
	case CV_8UC3:
		for(int r = rect.y, r_end = rect.y + rect.height; r < r_end; ++r)
		for(int c = rect.x, c_end = rect.x + rect.width;  c < c_end; ++c)
		{
			const cv::Vec3b& src_color = src.at<cv::Vec3b>(r - origin.y, c - origin.x);
			cv::Vec3b& dst_color = result.at<cv::Vec3b>(r, c);

			dst_color = venus::mix(dst_color, src_color, amount);
		}
		break;
#endif
	case CV_8UC4:
		for(int r = rect.y, r_end = rect.y + rect.height; r < r_end; ++r)
		for(int c = rect.x, c_end = rect.x + rect.width;  c < c_end; ++c)
		{
			const cv::Vec4b& src_color = src.at<cv::Vec4b>(r - origin.y, c - origin.x);
			cv::Vec4b& dst_color = result.at<cv::Vec4b>(r, c);

			dst_color = venus::mix(dst_color, src_color, amount);
		}
		break;
#if 0  // currently unused case
	case CV_32FC3:
		for(int r = rect.y, r_end = rect.y + rect.height; r < r_end; ++r)
		for(int c = rect.x, c_end = rect.x + rect.width;  c < c_end; ++c)
		{
			const cv::Vec3f& src_color = src.at<cv::Vec3f>(r - origin.y, c - origin.x);
			cv::Vec3f& dst_color = result.at<cv::Vec3f>(r, c);

			dst_color = venus::mix(dst_color, src_color, amount);
		}
		break;
#endif
	case CV_32FC4:
		for(int r = rect.y, r_end = rect.y + rect.height; r < r_end; ++r)
		for(int c = rect.x, c_end = rect.x + rect.width;  c < c_end; ++c)
		{
			const cv::Vec4f& src_color = src.at<cv::Vec4f>(r - origin.y, c - origin.x);
			cv::Vec4f& dst_color = result.at<cv::Vec4f>(r, c);

			dst_color = venus::mix(dst_color, src_color, amount);
		}
		break;
	default:
		assert(false);
		break;
	}
}

void Makeup::blend(cv::Mat& result, const cv::Mat& dst, const cv::Mat& src, const cv::Mat& mask, const cv::Point2i& origin, float amount)
{
	assert(src.channels() == dst.channels() && src.depth() == dst.depth());
	assert(mask.type() == CV_8UC1);
	dst.copyTo(result);

	Rect rect_src(origin.x, origin.y, src.cols, src.rows);
	Rect rect_dst(0, 0, dst.cols, dst.rows);
	Rect rect = rect_dst & rect_src;

	Rect2i rect_mask(0, 0, mask.cols, mask.rows);
	int offset_x = (src.cols - mask.cols)/2;
	int offset_y = (src.rows - mask.rows)/2;

	for(int r = rect.y, r_end = rect.y + rect.height; r < r_end; ++r)
	for(int c = rect.x, c_end = rect.x + rect.width;  c < c_end; ++c)
	{
		const int src_r = r - origin.y, src_c = c - origin.x;
		Point2i mask_position(src_c - offset_x, src_r - offset_y);
		if(!rect_mask.contains(mask_position) || mask.at<uint8_t>(mask_position) == 0)
			continue;

		switch(dst.type())
		{
		case CV_8UC4:
		{
			const cv::Vec4b& src_color = src.at<cv::Vec4b>(src_r, src_c);
			cv::Vec4b& dst_color = result.at<cv::Vec4b>(r, c);

			dst_color = venus::mix(dst_color, src_color, amount);
		}
			break;
		case CV_32FC3:
		{
			const cv::Vec3f& src_color = src.at<cv::Vec3f>(src_r, src_c);
			cv::Vec3f& dst_color = result.at<cv::Vec3f>(r, c);

			dst_color = venus::mix(dst_color, src_color, amount);
		}
			break;
		case CV_32FC4:
		{
			const cv::Vec4f& src_color = src.at<cv::Vec4f>(src_r, src_c);
			cv::Vec4f& dst_color = result.at<cv::Vec4f>(r, c);

			dst_color = venus::mix(dst_color, src_color, amount);
		}
			break;
		default:
			assert(false);
			break;
		}
	}
}

void Makeup::applyBrow(cv::Mat& dst, const cv::Mat& src, const std::vector<cv::Point2f>& points,
		const cv::Mat& mask, uint32_t color, float amount, float offsetY/* = 0.0F */)
{
	assert(src.type() == CV_8UC4 && mask.type() == CV_8UC1);
	if(src.data != dst.data)
		src.copyTo(dst);

	Feature feature(src, points);
	Vec4f line = feature.getSymmetryAxis();
	float angle = std::atan2(line[1], line[0]) - static_cast<float>(M_PI/2);
//	std::cout << __FUNCTION__ << " angle: " << rad2deg(angle) << '\n';

	const Mat&   makeup_mask = mask;
	const Rect2i makeup_rect = Region::boundingRect(makeup_mask, 4);  // mask image is not so good, so allow some tolerance.

	// centroid @see http://docs.opencv.org/2.4/doc/tutorials/imgproc/shapedescriptors/moments/moments.html
	Moments makeup_moment = cv::moments(mask);
	Point2f makeup_center(static_cast<float>(makeup_moment.m10 / makeup_moment.m00),
	                      static_cast<float>(makeup_moment.m01 / makeup_moment.m00));

	constexpr int offset = 8;  // TODO, tweak offset according to the eye brow size?
	const bool has_alpha = src.channels() > 3;

	for(int i = 0; i < 2; ++i)
	{
		const bool right = (i == 0);
		std::vector<Point2f> polygon = Feature::calculateBrowPolygon(points, right);
		Moments moment = cv::moments(polygon);
		const Point2f center(static_cast<float>(moment.m10 / moment.m00),
			                 static_cast<float>(moment.m01 / moment.m00));

		const Rect rect = cv::boundingRect(polygon);
		Rect rect_with_margin = rect;
		Region::inset(rect_with_margin, -offset);

		Mat roi = dst(rect_with_margin).clone();
		if(has_alpha)
			cv::cvtColor(roi, roi, CV_RGBA2RGB);  // or CV_BGRA2BGR, just strip alpha.

		Mat roi_mask = Feature::createMask(polygon);
		Mat roi_mask_with_margin(rect_with_margin.height, rect_with_margin.width, CV_8UC1, Scalar::all(0));
		roi_mask.copyTo(roi_mask_with_margin(Rect(offset, offset, roi_mask.cols, roi_mask.rows)));

#if USE_OPENCV_INPAINT
		constexpr double inpaint_radius = 10.0;  // TODO, make it adaptive, or tune it for a fine result.

		// tested with Navier-Stokes algorithm and A. Telea algorithm, and no obvious difference found.
		cv::inpaint(roi, roi_mask_with_margin, roi, inpaint_radius, cv::INPAINT_TELEA);
#else
		Mat source_mask;
		cv::bitwise_not(roi_mask_with_margin, source_mask);

		Inpainter inpainter;
		inpainter.setSourceImage(roi);
		inpainter.setSourceMask(source_mask);
		inpainter.setTargetMask(roi_mask_with_margin);
		inpainter.setPatchSize(4);
		inpainter.initialize();
	
		while(inpainter.hasMoreSteps())
			inpainter.step();

		inpainter.image().copyTo(roi);
#endif

#if 0
		// This branch will overwrite alpha channel value if @p src is not opaque(255).
		if(has_alpha)
			cv::cvtColor(roi, roi, CV_RGB2RGBA);  // recover alpha with full value(255).
		roi.copyTo(dst(rect_with_margin), roi_mask_with_margin);
#else
		// This branch keeps alpha channel untouched, so it's preferable.
		for(int r = 0; r < rect.height; ++r)
		for(int c = 0; c < rect.width;  ++c)
		{
			uint8_t alpha = roi_mask_with_margin.at<uint8_t>(r + offset, c + offset);
			if(alpha == 0)  // shortcut
				continue;

			const cv::Vec3b& src_color = roi.at<cv::Vec3b>(r, c);
			cv::Vec4b& dst_color = dst.at<cv::Vec4b>(r + rect.y, c + rect.x);

//			*reinterpret_cast<cv::Vec3b*>(&dst_color) = src_color;
			// mixing(below) seems better than just overwriting(above).
			uint8_t* p = reinterpret_cast<uint8_t*>(&dst_color);
			p[0] = lerp(p[0], src_color[0], alpha);
			p[1] = lerp(p[1], src_color[1], alpha);
			p[2] = lerp(p[2], src_color[2], alpha);
		}
#endif

		if(!right)  // mirror image for left side
		{
			cv::flip(makeup_mask, makeup_mask, 1/* horizontal */);
			makeup_center.x = makeup_rect.width - makeup_center.x;
		}

		Vec2f scale(static_cast<float>(rect.width) / makeup_rect.width,
		            static_cast<float>(rect.height)/ makeup_rect.height);
		Size target_size = makeup_rect.size();
		Point2f target_center = makeup_center;
		Mat affine = Region::transform(target_size, target_center, angle, scale);
		cv::Mat affined_mask;
		cv::warpAffine(makeup_mask, affined_mask, affine, target_size, cv::INTER_LINEAR, cv::BORDER_CONSTANT);

		// need to move X coordinate with respect to the 1/slant.
		Point2f translation(offsetY/line[1] * line[0], offsetY);

		Mat affined_brow = Makeup::pack(affined_mask, color);
		Point2f origin = center - target_center + translation;
		Makeup::blend(dst, dst, affined_brow, origin, amount);
	}
}

void Makeup::applyEye(cv::Mat& dst, const cv::Mat& src, const std::vector<cv::Point2f>& points, const cv::Mat& cosmetic, float amount)
{
	assert(src.type() == CV_8UC4 && cosmetic.type() == CV_8UC4);
	src.copyTo(dst);

/*
	Below are eye feature point indices:


				36                    46
			 37    35              45    47
	right  38   42   34 -------- 44   43   48   left
			 39    41              51    49
				40                    50

*/
#if 1
	// I rearrange eye lashes into file doc/eye_lash.xcf
	const std::vector<Point2f> src_points  // corresponding index 34~41
	{
		Point2f(633, 287), Point2f(534, 228), Point2f(458, 213), Point2f(386, 228),
		Point2f(290, 287), Point2f(386, 350), Point2f(458, 362), Point2f(534, 353),
	};

	constexpr int N = 41 - 34 + 1;
	std::vector<Point2f> dst_points(N);

	auto calcuateEyeParams = [](const Point2f& right, const Point2f& left) -> cv::Vec4f
	{
		Point2f pivot = (right + left)/2;
		float radius = venus::distance(pivot, left);

		Point2f delta = right - left;
		if(delta.x < 0)
			delta = -delta;  // map angle into interval [-pi/2, pi/2]
		float angle = std::atan2(delta.y, delta.x);

		return Vec4f(pivot.x, pivot.y, radius, angle);
	};

	const Vec4f PARAMS = calcuateEyeParams(src_points[0], src_points[4]);

	for(int j = 0; j <= 1; ++j)
	{
		const bool right = (j == 0);
		const int  START = right ? 34:44;
		
		// for right: 34 35 36 37 38 39 40 41, formular 34 + i;
		// for left : 48 47 46 45 44 51 50 49, formular 44 + (12 - i)%8;
		if(right)
			for(int i = 0; i < N; ++i)
				dst_points[i] = points[34 + i];
		else
		{
			const float sum = points[44].x + points[48].x;  // only flip horizontally
			for(int i = 0; i < N; ++i)
				dst_points[i] = Point2f(sum - points[44 + i].x, points[44 + i].y);
		}

		Vec4f params = calcuateEyeParams(dst_points[0], dst_points[4]);
		printf("pivot: (%f, %f), radius: %f, angle: %f\n", params[0], params[1], params[2], rad2deg(params[3]));

		Size size(cosmetic.cols, cosmetic.rows);
		Point2f pivot(PARAMS[0], PARAMS[1]);
		float angle = params[3];
		float scale = params[2]/PARAMS[2];
		
		Mat affine = Region::transform(size, pivot, angle, Point2f(scale, scale));

		cv::Mat _cosmetic;
		cv::warpAffine(cosmetic, _cosmetic, affine, size, cv::INTER_LANCZOS4, cv::BORDER_CONSTANT);
		
		std::vector<Point2f> affined_src_points;
		cv::transform(src_points, affined_src_points, affine);
		pivot = Region::transform(affine, Point2f(PARAMS[0], PARAMS[1]));

		// and then move points to make src_points and dst_points' pivots coincide.
		const Point2f dst_pivot = (points[START] + points[START+4])/2;
		const Point2f offset = (affined_src_points[0] + affined_src_points[4])/2 - dst_pivot;
		for(size_t i = 0; i < N; ++i)
			dst_points[i] = dst_points[i] + offset;

		ImageWarp_Rigid warp;
		warp.setMappingPoints(dst_points, affined_src_points);
		warp.setSourceSize(_cosmetic.cols, _cosmetic.rows);
		warp.setTargetSize(_cosmetic.cols, _cosmetic.rows);
		warp.calculateDelta(1.0F);
		_cosmetic = warp.genNewImage(_cosmetic, 1.0F);
//		cv::imshow("test" + std::to_string(j), _cosmetic);

		if(!right)
		{
			// NOTICE the -1 here, since left(0) + right(cols - 1) == cols - 1.
			pivot.x = static_cast<float>(_cosmetic.cols - 1) - pivot.x;
			cv::flip(_cosmetic, _cosmetic, 1/* horizontally */);
		}
		Point2i origin = dst_pivot - pivot;

		Makeup::blend(dst, dst, _cosmetic, origin, amount);
	}
#else
	const Point2f LEFT(284, 287), RIGHT(633, 287);
	const Point2f TOP(458, 213), BOTTOM(458, 362);
	Point2f PIVOT, pivot;
	Vec4f DISTANCE = Feature::calcuateDistance(PIVOT, LEFT, TOP, RIGHT, BOTTOM);

	Feature feature(src, points);

	for(int i = 0; i <= 1; ++i)
	{
		const bool is_right = (i == 0);
		Vec4f distance = feature.calcuateEyeRadius(is_right);

		Vec4f scale;
		for(int i = 0; i < 4; ++i)  // sighs, no operator / overloaded for Vec4f.
			scale[i] = distance[i] / DISTANCE[i];
//		std::cout << "scale factor left: " << scale[0] << " top: " << scale[1] << " right: " << scale[2] << " bottom: " << scale[3] << '\n';

		Mat _cosmetic = cosmetic;

		if(!is_right)  // mirror for left part
		{
			std::swap(scale[0], scale[2]);  // std::swap(left, right);
			PIVOT.x = static_cast<float>(cosmetic.cols - 1) - PIVOT.x;  // left + right == width - 1
			cv::flip(_cosmetic, _cosmetic, 1/* horizontally */);
		}

		// use INTER_LANCZOS4 instead of INTER_LINEAR for best anti-aliasing result
		_cosmetic = Region::resize(_cosmetic, PIVOT, scale, INTER_LANCZOS4);

		// make pivot coincides
		Region region = feature.calculateEyeRegion(is_right);
		Rect rect = region.getRect();
		Point2f position(rect.x - (_cosmetic.cols - rect.width )/2.0F,
			             rect.y - (_cosmetic.rows - rect.height)/2.0F);

		// rotate if skew too much

		Makeup::blend(dst, dst, _cosmetic, position, amount);
	}
#endif
}

void Makeup::applyEyeLash(cv::Mat& dst, const cv::Mat& src, const std::vector<cv::Point2f>& points, const cv::Mat& mask, uint32_t color, float amount)
{
	assert(mask.type() == CV_8UC1);
	Mat eye_lash = pack(mask, color);

	applyEye(dst, src, points, eye_lash, amount);
}

cv::Mat Makeup::createEyeShadow(cv::Mat mask[3], uint32_t color[3]/*, const int& COUNT = 3 */)
{
	const int cols = mask[0].cols, rows = mask[0].rows;
	Mat bitmap(rows, cols, CV_8UC4);
	constexpr int COUNT = 3;

	auto unpack = [](uint32_t c) -> cv::Vec3i { return cv::Vec3i(c & 0xFF, (c>>8) & 0xFF, (c>>16) & 0xFF/*, (c>>24) & 0xFF*/); };
	const Vec3i _color[COUNT] = { unpack(color[0]), unpack(color[1]), unpack(color[2]) };
	const Vec3i _127(127, 127, 127);

	// note that blending mode can be tweaked!
	for(int r = 0; r < rows; ++r)
	for(int c = 0; c < cols; ++c)
	{
		Vec3i rgb(0, 0, 0);
		int a = 0, a_max = 0;
		for(int i = 0; i < COUNT; ++i)
		{
			uint8_t alpha = mask[i].at<uint8_t>(r, c);
			rgb += _color[i] * alpha;
			a   += alpha;

			if(a_max < alpha)
				a_max = alpha;
		}
		if(a != 0)
			rgb = rgb/a;//(rgb + _127) / 255;
		a   = a_max;//(a + COUNT/2)/COUNT;

		for(int i = 0; i < 3; ++i)
			assert(0 <= rgb[i] && rgb[i] <= 255);

		bitmap.at<Vec4b>(r, c) = Vec4b(
#if USE_BGRA_LAYOUT
			rgb[2], rgb[1], rgb[0],
#else
			rgb[0], rgb[1], rgb[2],
#endif
			a);
	}

	return bitmap;
}

void Makeup::applyEyeShadow(cv::Mat& dst, const cv::Mat& src, const std::vector<cv::Point2f>& points, cv::Mat mask[3], uint32_t color[3], float amount)
{
	Mat eye_shadow = createEyeShadow(mask, color);
	applyEye(dst, src, points, eye_shadow, amount);
}

void Makeup::applyBlush(cv::Mat& dst, const cv::Mat& src, const std::vector<cv::Point2f>& points, BlushShape shape, uint32_t color, float amount)
{
	assert(!src.empty() && points.size() == Feature::COUNT);
	assert(0 <= amount && amount <= 1);

	src.copyTo(dst);

	for(int i = 0; i <= 1; ++i)  // i == 0 for left cheek, i == 1 for right cheek
	{
		// static_cast<bool>(i) emits warning "C4800: 'int' : forcing value to bool 'true' or 'false' (performance warning)".
		// But why it says performance warning?
		// http://stackoverflow.com/questions/206564/what-is-the-performance-implication-of-converting-to-bool-in-c
		std::vector<Point2f> polygon = createPolygon(points, shape, i != 0);

		Rect rect = cv::boundingRect(polygon);
		Mat  mask = Feature::maskPolygonSmooth(rect, polygon, 8);  // level (here 8) can be tuned.
		Mat blush = pack(mask, color);
//		cv::imshow(std::string("blush mask ") + (i == 0 ? "left":"right"), mask);
		blend(dst, dst, blush, rect.tl(), amount);
	}
}

void Makeup::applyLip(cv::Mat& dst, const cv::Mat& src, const std::vector<cv::Point2f>& points, uint32_t color, float amount)
{
	assert(!src.empty() && src.channels() == 4);  // only handles RGBA image

	Feature feature(src, points);
	Region region = feature.calculateLipshRegion();
	const Mat& mask = region.mask;
	const Point2f& pivot = region.pivot;
	const int& rows = mask.rows, &cols = mask.cols;
	const Point2i& origin = pivot - static_cast<Point2f>(Point2i(mask.cols, mask.rows))/2;
	
/*
	Rect all(0, 0, src.cols, src.rows);
	Rect lip(origin.x, origin.y, rows, cols);
	const Rect rect = all & lip;
	
	if(rect.area() <= 0)  // not overlap
		return;
*/
	cv::Mat lip(rows, cols, CV_8UC4, Scalar::all(0));
	uint32_t* lip_data = reinterpret_cast<uint32_t*>(lip.data);
	for(int i = 0, length = rows * cols; i < length; ++i)
		lip_data[i] = color;

#if 1
	blend(dst, src, lip, mask, origin, amount);
#else
	// SRC_OVER mode, [Sa + (1 - Sa)*Da, Rc = Sc + (1 - Sa)*Dc]
	const float l_amount = 1 - amount;
	if(src.type() == CV_8UC4)
	{
		float color_0_x_amount = (color      &0xff) * amount;
		float color_1_x_amount = ((color>> 8)&0xff) * amount;
		float color_2_x_amount = ((color>>16)&0xff) * amount;
		float color_3_x_amount = ((color>>24)&0xff) * amount;

//		uint8_t color_0 = color, color_1 = color>>8, color_2 = color>>16, color_3 = color>>24;
		for(int r = rect.y, r_end = rect.y + rect.height; r < r_end; ++r)
		for(int c = rect.x, c_end = rect.x + rect.width;  c < c_end; ++c)
		{
			uint8_t mask = region.mask.at<uint8_t>(r - rect_mask.y, c - rect_mask.x);
			if(mask == 0)  // skip transparent area
				continue;

			const Vec4b& src_color = src.at<Vec4b>(r, c);
//			uint8_t src_0 = src_color, src_1 = src_color>>8, src_2 = src_color>>16, src_3 = src_color>>24;
//			uint32_t mixed = mix(src_color, color, amount);
			uint8_t _0 = saturate_cast<uint8_t>((src_color[0] & 0xff) * l_amount + color_0_x_amount);
			uint8_t _1 = saturate_cast<uint8_t>((src_color[1] & 0xff) * l_amount + color_1_x_amount);
			uint8_t _2 = saturate_cast<uint8_t>((src_color[2] & 0xff) * l_amount + color_2_x_amount);
			uint8_t _3 = saturate_cast<uint8_t>((src_color[3] & 0xff) * l_amount + color_3_x_amount);

			uint8_t dst_0 = (src_color[0] * (255 - mask) + _0 * mask + 127) / 255;
			uint8_t dst_1 = (src_color[1] * (255 - mask) + _1 * mask + 127) / 255;
			uint8_t dst_2 = (src_color[2] * (255 - mask) + _2 * mask + 127) / 255;
			uint8_t dst_3 = _3; // (src_color[3] * (255 - mask) + _3 * mask + 127) / 255;

//			uint32_t dst_color = dst_0 || (dst_1 << 8) || (dst_2 << 16) || (dst_3 << 24);
			dst.at<Vec4b>(r, c) = Vec4b(dst_0, dst_1, dst_2, dst_3);
		}
	}
	else if(src.type() == CV_32FC4)
	{
		float color_0_x_amount = (color      &0xff) * (amount / 255.0f);
		float color_1_x_amount = ((color>> 8)&0xff) * (amount / 255.0f);
		float color_2_x_amount = ((color>>16)&0xff) * (amount / 255.0f);
		float color_3_x_amount = ((color>>24)&0xff) * (amount / 255.0f);
		for(int r = rect.y, r_end = rect.y + rect.height; r < r_end; ++r)
		for(int c = rect.x, c_end = rect.x + rect.width;  c < c_end; ++c)
		{
			float mask = region.mask.at<float>(r - rect_mask.y, c - rect_mask.x);
			float l_mask = 1 - mask;
			const Vec4f& src_color = src.at<Vec4f>(r, c);
			float _0 = src_color[0] * l_amount + color_0_x_amount;
			float _1 = src_color[1] * l_amount + color_1_x_amount;
			float _2 = src_color[2] * l_amount + color_2_x_amount;
			float _3 = src_color[3] * l_amount + color_3_x_amount;

			Vec4f& dst_color = dst.at<Vec4f>(r, c);
			dst_color[0] = src_color[0] * l_mask + _0 * mask;
			dst_color[1] = src_color[1] * l_mask + _1 * mask;
			dst_color[2] = src_color[2] * l_mask + _2 * mask;
			dst_color[3] = src_color[3] * l_mask + _3 * mask;
		}
	}
	else
		assert(false);  // unimplemented yet
#endif
}

} /* namespace venus */