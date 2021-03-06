#include "stdafx.h"
#include <set>
#include <cstdio>
#include "piecewiseAffineWarpAAModel.h"
#include "textureDistributionAAModel.h"

#if CV_MAJOR_VERSION > 1
#include "opencv2\legacy\legacy.hpp"
#endif

#define BINLINEAR 1

using namespace std;

#define free2dvector(vec)										\
{																\
	for(int i = 0; i < vec.size(); i++) vec[i].clear();			\
	vec.clear();												\
}

inline int pointPolygonTest2(CvPoint2D32f cntf[3], CvPoint pt) {
	int result = 0;
	int i, total = 3, counter = 0;
	CvPoint2D32f v0, v = cntf[total - 1];

	for (i = 0; i < total; i++) {
		double dist;
		v0 = v;
		v = cntf[i];
		if ((v0.y <= pt.y && v.y <= pt.y) || (v0.y > pt.y && v.y > pt.y) || (v0.x < pt.x && v.x < pt.x)) {
			if (pt.y == v.y && (pt.x == v.x || (pt.y == v0.y &&
				((v0.x <= pt.x && pt.x <= v.x) || (v.x <= pt.x && pt.x <= v0.x)))))
				return 0;
			continue;
		}
		dist = (double)(pt.y - v0.y)*(v.x - v0.x) - (double)(pt.x - v0.x)*(v.y - v0.y);
		if (dist == 0)
			return 0;
		if (v.y < v0.y)
			dist = -dist;
		counter += dist > 0;
	}
	result = counter % 2 == 0 ? -1 : 1;

	return result;
}

piecewiseAffineWarpAAModel::piecewiseAffineWarpAAModel() {
	__trianglesCount = 0;
	__nPixels = 0;
	__pointsCount = 0;
	__width = 0;
	__height = 0;
}

piecewiseAffineWarpAAModel::~piecewiseAffineWarpAAModel() {
	__pixTri.clear();
	__alpha.clear();
	__belta.clear();
	__gamma.clear();

	free2dvector(__rect);
	free2dvector(__vtri);
	free2dvector(__triangles);
}

void piecewiseAffineWarpAAModel::trainModel(const ShapeModel& ReferenceShape, CvMat* Points, CvMemStorage* Storage, const std::vector<std::vector<int> >* tri,
	bool buildVtri) {
	__referenceshape = ReferenceShape;
	__pointsCount = __referenceshape.getPointsCount();

	for (int i = 0; i < __pointsCount; i++)
		CV_MAT_ELEM(*Points, CvPoint2D32f, 0, i) = __referenceshape[i];

	CvMat* convexHullMat = cvCreateMat(1, __pointsCount, CV_32FC2);
	cvConvexHull2(Points, convexHullMat, CV_CLOCKWISE, 0);

	CvRect boundingRect = cvBoundingRect(convexHullMat, 0);
	CvSubdiv2D* Subdiv2D = cvCreateSubdivDelaunay2D(boundingRect, Storage);
	for (int ii = 0; ii < __pointsCount; ii++)
		cvSubdivDelaunay2DInsert(Subdiv2D, __referenceshape[ii]);

	// Building the triangles first
	if (tri == 0) Delaunay(Subdiv2D, convexHullMat);
	else __triangles = *tri;
	__trianglesCount = __triangles.size();

	// Result 3 --- delunay triangulation
	/* IplImage* imgScribble = cvCreateImage(cvSize(1000, 1000), 8, 3);
	cvSet(imgScribble, cvScalar(0, 0, 0));
	cvNamedWindow("tempWin", CV_WINDOW_AUTOSIZE);
	Commons::DrawTriangles(imgScribble, ReferenceShape, __triangles);
	cvShowImage("tempWin", imgScribble);
	cvWaitKey(0);
	cvReleaseImage(&imgScribble); */

	//secondly: build correspondence of Vertex-Triangle
	if (buildVtri) FindVTri();

	//Thirdly: build pixel point in all triangles
	if (tri == 0) computePixelPoint(boundingRect, convexHullMat);
	else FastCalcPixelPoint(boundingRect);
	__nPixels = __pixTri.size();// get the number of pixels

	cvReleaseMat(&convexHullMat);
}

void piecewiseAffineWarpAAModel::Delaunay(const CvSubdiv2D* Subdiv2D, const CvMat* convexHullMat) {
	// Build edges initially
	int i;
	CvSeqReader  reader;
	CvQuadEdge2D* edge;
	CvPoint2D32f org, dst;
	CvSubdiv2DPoint* org_pt, *dst_pt;
	std::vector<std::vector<int> > edges;
	std::vector<int> one_edge;     one_edge.resize(2);
	std::vector<int> one_tri;	one_tri.resize(3);
	int ind1, ind2;

	cvStartReadSeq((CvSeq*)(Subdiv2D->edges), &reader, 0);
	for (i = 0; i < Subdiv2D->edges->total; i++) {
		edge = (CvQuadEdge2D*)(reader.ptr);
		if (CV_IS_SET_ELEM(edge)) {
			org_pt = cvSubdiv2DEdgeOrg((CvSubdiv2DEdge)edge);
			dst_pt = cvSubdiv2DEdgeDst((CvSubdiv2DEdge)edge);

			if (org_pt && dst_pt) {
				org = org_pt->pt;
				dst = dst_pt->pt;
				if (cvPointPolygonTest(convexHullMat, org, 0) >= 0 &&
					cvPointPolygonTest(convexHullMat, dst, 0) >= 0) {
					for (int j = 0; j < __pointsCount; j++) {
						if (fabs(org.x - __referenceshape[j].x)<1e-6 && fabs(org.y - __referenceshape[j].y)<1e-6) {
							for (int k = 0; k < __pointsCount; k++) {
								if (fabs(dst.x - __referenceshape[k].x)<1e-6 &&fabs(dst.y - __referenceshape[k].y)<1e-6) {
									one_edge[0] = j;
									one_edge[1] = k;
									edges.push_back(one_edge);
								}
							}
						}
					}
				}
			}
		}

		CV_NEXT_SEQ_ELEM(Subdiv2D->edges->elem_size, reader);
	}

	// Build triangles now
	for (i = 0; i < edges.size(); i++) {
		ind1 = edges[i][0];
		ind2 = edges[i][1];

		for (int j = 0; j < __pointsCount; j++) {
			if (piecewiseAffineWarpAAModel::IsEdgeIn(ind1, j, edges) && piecewiseAffineWarpAAModel::IsEdgeIn(ind2, j, edges)) {
				one_tri[0] = ind1;
				one_tri[1] = ind2;
				one_tri[2] = j;
				if (piecewiseAffineWarpAAModel::IsTriangleNotIn(one_tri, __triangles)) {
					__triangles.push_back(one_tri);
				}
			}
		}
	}
}

bool piecewiseAffineWarpAAModel::IsEdgeIn(int ind1, int ind2, const std::vector<std::vector<int> > &edges) {
	for (int i = 0; i < edges.size(); i++) {
		if ((edges[i][0] == ind1 && edges[i][1] == ind2) || (edges[i][0] == ind2 && edges[i][1] == ind1)) return true;
	}
	return false;
}

bool piecewiseAffineWarpAAModel::IsTriangleNotIn(const std::vector<int>& one_tri, const std::vector<std::vector<int> > &tris) {
	std::set<int> tTriangle;
	std::set<int> sTriangle;

	for (int i = 0; i < tris.size(); i++) {
		tTriangle.clear();
		sTriangle.clear();
		for (int j = 0; j < 3; j++) {
			tTriangle.insert(tris[i][j]);
			sTriangle.insert(one_tri[j]);
		}
		if (tTriangle == sTriangle)    return false;
	}

	return true;
}

void piecewiseAffineWarpAAModel::computePixelPoint(const CvRect boundingRect, CvMat* convexHullMat) {
	CvPoint2D32f point[3];
	CvMat tempVert = cvMat(1, 3, CV_32FC2, point);
	int ll = 0;
	double alpha, belta, gamma;
	CvPoint2D32f pt;
	int ind1, ind2, ind3;
	int ii, jj;
	double x, y, x1, y1, x2, y2, x3, y3, c;

	__xmin = boundingRect.x;
	__ymin = boundingRect.y;
	__width = boundingRect.width;
	__height = boundingRect.height;
	int left = boundingRect.x, right = left + __width;
	int top = boundingRect.y, bottom = top + __height;

	__rect.resize(__height);
	for (int i = top; i < bottom; i++) {
		ii = i - top;
		__rect[ii].resize(__width);
		pt.y = i;
		for (int j = left; j < right; j++) {
			jj = j - left;
			//pt = cvPoint2D32f(j, i);
			pt.x = j;
			__rect[ii][jj] = -1;

			// firstly: the point (j, i) is located in the convexHullMat
			if (cvPointPolygonTest(convexHullMat, pt, 0) >= 0) {
				// then we find the triangle that the point lies in
				for (int k = 0; k < __trianglesCount; k++) {
					ind1 = __triangles[k][0];
					ind2 = __triangles[k][1];
					ind3 = __triangles[k][2];
					point[0] = __referenceshape[ind1];
					point[1] = __referenceshape[ind2];
					point[2] = __referenceshape[ind3];

					// secondly: the point(j,i) is located in the k-th triangle
					if (cvPointPolygonTest(&tempVert, pt, 0) >= 0) {
						__rect[ii][jj] = ll++;
						__pixTri.push_back(k);

						// calculate alpha and belta for warp
						x = j;		 y = i;
						x1 = point[0].x; y1 = point[0].y;
						x2 = point[1].x; y2 = point[1].y;
						x3 = point[2].x; y3 = point[2].y,

							c = 1.0 / (+x2*y3 - x2*y1 - x1*y3 - x3*y2 + x3*y1 + x1*y2);
						alpha = (y*x3 - y3*x + x*y2 - x2*y + x2*y3 - x3*y2)*c;
						belta = (-y*x3 + x1*y + x3*y1 + y3*x - x1*y3 - x*y1)*c;
						gamma = 1 - alpha - belta;

						__alpha.push_back(alpha);
						__belta.push_back(belta);
						__gamma.push_back(gamma);

						// make sure each point only located in only one triangle
						break;
					}
				}
			}
		}
	}
}

enum { XY_SHIFT = 16, XY_ONE = 1 << XY_SHIFT, DRAWING_STORAGE_BLOCK = (1 << 12) - 256 };

struct one_edge {
	int y;
	int xmin, xmax;

	one_edge() : y(0), xmin(0), xmax(0) {}
	one_edge(int y, int xmin, int xmax) : y(y), xmin(xmin), xmax(xmax) {}
};

int piecewiseAffineWarpAAModel::FastFillConvexPoly(CvPoint2D32f pts[3], void* data) {
	struct {
		int idx, di;
		int x, dx, ye;
	}
	edge[2];

	CvPoint v[] = { cvPointFrom32f(pts[0]),cvPointFrom32f(pts[1]), cvPointFrom32f(pts[2]) };
	int npts = 3;
	int shift = 0;
	int delta = shift ? 1 << (shift - 1) : 0;
	int i, y, imin = 0, left = 0, right = 1, xX1, xX2;
	int edges = npts;
	int xmin, xmax, ymin, ymax;
	CvPoint p0;
	int delta1, delta2;
	int ycount = 0;
	one_edge* idx = (one_edge*)data;

	delta1 = delta2 = XY_ONE >> 1;

	p0 = v[npts - 1];
	p0.x <<= XY_SHIFT - shift;
	p0.y <<= XY_SHIFT - shift;

	xmin = xmax = v[0].x;
	ymin = ymax = v[0].y;

	for (i = 0; i < npts; i++) {
		CvPoint p = v[i];
		if (p.y < ymin) {
			ymin = p.y;
			imin = i;
		}

		ymax = MAX(ymax, p.y);
		xmax = MAX(xmax, p.x);
		xmin = MIN(xmin, p.x);

		p.x <<= XY_SHIFT - shift;
		p.y <<= XY_SHIFT - shift;

		CvPoint pt0, pt1;
		pt0.x = p0.x >> XY_SHIFT;
		pt0.y = p0.y >> XY_SHIFT;
		pt1.x = p.x >> XY_SHIFT;
		pt1.y = p.y >> XY_SHIFT;

		p0 = p;
	}

	xmin = (xmin + delta) >> shift;
	xmax = (xmax + delta) >> shift;
	ymin = (ymin + delta) >> shift;
	ymax = (ymax + delta) >> shift;

	edge[0].idx = edge[1].idx = imin;

	edge[0].ye = edge[1].ye = y = ymin;
	edge[0].di = 1;
	edge[1].di = npts - 1;

	do {
		if (y < ymax || y == ymin) {
			for (i = 0; i < 2; i++) {
				if (y >= edge[i].ye) {
					int idx = edge[i].idx, di = edge[i].di;
					int xs = 0, xe, ye, ty = 0;

					for (;;) {
						ty = (v[idx].y + delta) >> shift;
						if (ty > y || edges == 0) break;
						xs = v[idx].x;
						idx += di;
						idx -= ((idx < npts) - 1) & npts;   /* idx -= idx >= npts ? npts : 0 */
						edges--;
					}

					ye = ty;
					xs <<= XY_SHIFT - shift;
					xe = v[idx].x << (XY_SHIFT - shift);

					/* no more edges */
					if (y >= ye)
						return ycount;

					edge[i].ye = ye;
					edge[i].dx = ((xe - xs) * 2 + (ye - y)) / (2 * (ye - y));
					edge[i].x = xs;
					edge[i].idx = idx;
				}
			}
		}

		if (edge[left].x > edge[right].x) {
			left ^= 1;
			right ^= 1;
		}

		xX1 = edge[left].x;
		xX2 = edge[right].x;

		if (y >= 0) {
			int xx1 = (xX1 + delta1) >> XY_SHIFT;
			int xx2 = (xX2 + delta2) >> XY_SHIFT;

			if (xx2 >= 0) {
				if (xx1 < 0)
					xx1 = 0;
				idx[ycount].y = y;
				idx[ycount].xmin = xx1;
				idx[ycount].xmax = xx2;
				ycount++;
			}
		}

		xX1 += edge[left].dx;
		xX2 += edge[right].dx;

		edge[left].x = xX1;
		edge[right].x = xX2;
	} while (++y <= ymax);
	return ycount;
}

void piecewiseAffineWarpAAModel::FastCalcPixelPoint(const CvRect boundingRect) {
	CvPoint2D32f point[3];
	CvMat oneTri = cvMat(1, 3, CV_32FC2, point);
	double alpha, belta, gamma;
	CvPoint pt;
	int ind1, ind2, ind3;
	int ll = 0;
	int x, y;
	double x1, y1, x2, y2, x3, y3, c;
	struct one_edge* idx = NULL;

	__xmin = boundingRect.x;			__ymin = boundingRect.y;
	__width = boundingRect.width + 1;		__height = boundingRect.height + 1;
	int left = boundingRect.x, top = boundingRect.y;
	int aa, bb, cc, dd;
	__rect.resize(__height);
	for (int i = 0; i < __height; i++) {
		__rect[i].resize(__width);
		for (int j = 0; j < __width; j++)
			__rect[i][j] = -1;
	}

	for (int k = 0; k < __trianglesCount; ++k) {
		ind1 = __triangles[k][0];
		ind2 = __triangles[k][1];
		ind3 = __triangles[k][2];

		point[0] = __referenceshape[ind1];
		point[1] = __referenceshape[ind2];
		point[2] = __referenceshape[ind3];

		x1 = point[0].x; y1 = point[0].y;
		x2 = point[1].x; y2 = point[1].y;
		x3 = point[2].x; y3 = point[2].y;
		c = 1.0 / (+x2*y3 - x2*y1 - x1*y3 - x3*y2 + x3*y1 + x1*y2);

#if 1
		if (idx == NULL) idx = new one_edge[__height];
		int ycount = FastFillConvexPoly(point, idx);
		if (ycount == 0) continue;
		cc = idx[0].y;
		dd = ycount == 1 ? cc : idx[ycount - 1].y;
		for (y = cc; y <= dd; ++y) {
			int Y = y - cc;
			for (x = idx[Y].xmin; x <= idx[Y].xmax; ++x) {
				__rect[y - top][x - left] = ll++;
				__pixTri.push_back(k);

				alpha = (y*(x3 - x2) + x2*y3 - x3*y2 + x*(y2 - y3))*c;
				belta = (y*(x1 - x3) + x3*y1 - x1*y3 + (y3 - y1)*x)*c;
				gamma = 1 - alpha - belta;

				__alpha.push_back(alpha);
				__belta.push_back(belta);
				__gamma.push_back(gamma);

			}
		}
#else
		aa = int(MIN(x1, MIN(x2, x3)) + 0.5); //left x
		bb = MAX(x1, MAX(x2, x3)); //right x
		cc = int(MIN(y1, MIN(y2, y3)) + 0.5); //top y
		dd = MAX(y1, MAX(y2, y3)); //bot y

		for (y = cc; y <= dd; ++y)
		{
			pt.y = y;
			for (x = aa; x <= bb; ++x)
			{
				pt.x = x;
				//the point is located in the k-th triangle
				if (pointPolygonTest2(point, pt) >= 0)
				{
					__rect[y - top][x - left] = ll++;
					__pixTri.push_back(k);

					//alpha = (y*x3-y3*x+x*y2-x2*y+x2*y3-x3*y2)*c;
					//belta = (-y*x3+x1*y+x3*y1+y3*x-x1*y3-x*y1)*c;
					alpha = (y*(x3 - x2) + x2*y3 - x3*y2 + x*(y2 - y3))*c;
					belta = (y*(x1 - x3) + x3*y1 - x1*y3 + (y3 - y1)*x)*c;
					gamma = 1 - alpha - belta;

					__alpha.push_back(alpha);
					__belta.push_back(belta);
					__gamma.push_back(gamma);
				}

			}
		}
#endif
	}
	delete[]idx;
}

void piecewiseAffineWarpAAModel::FindVTri() {
	__vtri.resize(__pointsCount);
	for (int i = 0; i < __pointsCount; i++) {
		for (int j = 0; j < __trianglesCount; j++) {
			if (__triangles[j][0] == i || __triangles[j][1] == i || __triangles[j][2] == i)
				__vtri[i].push_back(j);
		}
	}
}

void piecewiseAffineWarpAAModel::computeWarpedParams(double x, double y, double x1, double y1, double x2, double y2, double x3, double y3,
	double &alpha, double &belta, double &gamma) {
	double c = (+x2*y3 - x2*y1 - x1*y3 - x3*y2 + x3*y1 + x1*y2);
	alpha = (y*x3 - y3*x + x*y2 - x2*y + x2*y3 - x3*y2) / c;
	belta = (-y*x3 + x1*y + x3*y1 + y3*x - x1*y3 - x*y1) / c;
	gamma = 1 - alpha - belta;
}

void piecewiseAffineWarpAAModel::Warp(double x, double y, double x1, double y1, double x2, double y2, double x3, double y3,
	double& X, double& Y, double X1, double Y1, double X2, double Y2, double X3, double Y3) {
	double c = 1.0 / (+x2*y3 - x2*y1 - x1*y3 - x3*y2 + x3*y1 + x1*y2);
	double alpha = (y*x3 - y3*x + x*y2 - x2*y + x2*y3 - x3*y2)*c;
	double belta = (-y*x3 + x1*y + x3*y1 + y3*x - x1*y3 - x*y1)*c;
	double gamma = 1.0 - alpha - belta;

	X = alpha*X1 + belta*X2 + gamma*X3;
	Y = alpha*Y1 + belta*Y2 + gamma*Y3;
}

void piecewiseAffineWarpAAModel::convertTextureToImage(IplImage* image, const CvMat* t)const {
	CvMat* tt = cvCloneMat(t);
	double minV, maxV;
	cvMinMaxLoc(tt, &minV, &maxV);
	cvConvertScale(tt, tt, 255 / (maxV - minV), -minV * 255 / (maxV - minV));

	int k, x3;
	double *T = tt->data.db;
	byte* p;

	for (int y = 0; y < __height; y++) {
		p = (byte*)(image->imageData + image->widthStep*y);
		for (int x = 0; x < __width; x++) {
			k = __rect[y][x];
			if (k >= 0) {
				x3 = x + (x << 1); k = k + (k << 1);
				p[x3] = T[k];
				p[x3 + 1] = T[k + 1];
				p[x3 + 2] = T[k + 2];
			}
		}
	}
	cvReleaseMat(&tt);
}

void piecewiseAffineWarpAAModel::storeWarpedTextureAsImage(const char* filename, const CvMat* t)const {
	IplImage* image = cvCreateImage(cvSize(__width, __height), IPL_DEPTH_8U, 3);
	cvSetZero(image);
	convertTextureToImage(image, t);
	LOGI("Saving Warped Texture as Image...\n", image);
	cvSaveImage(filename, image);
	cvReleaseImage(&image);
}

void piecewiseAffineWarpAAModel::computeWarpedTexture(const CvMat* s, const IplImage* image, CvMat* t)const {
	double *fastt = t->data.db;
	double *ss = s->data.db;
	int v1, v2, v3, tri_idx;
	double x, y;
	int X, Y, X1, Y1;
	double s0, t0, s1, t1;
	int ixB1, ixG1, ixR1, ixB2, ixG2, ixR2;
	byte* p1, *p2;
	//	byte ltb, ltg, ltr, lbb, lbg, lbr, rtb, rtg, rtr, rbb, rbg, rbr;
	//	double b1 , b2, g1, g2 , r1, r2;
	char* imgdata = image->imageData;
	int step = image->widthStep;
	int nchannel = image->nChannels;
	if (nchannel == 4) {
		for (int i = 0, k = 0; i < __nPixels; i++, k += 3) {
			tri_idx = __pixTri[i];
			v1 = __triangles[tri_idx][0];
			v2 = __triangles[tri_idx][1];
			v3 = __triangles[tri_idx][2];

			x = __alpha[i] * ss[v1 << 1] + __belta[i] * ss[v2 << 1] +
				__gamma[i] * ss[v3 << 1];
			y = __alpha[i] * ss[1 + (v1 << 1)] + __belta[i] * ss[1 + (v2 << 1)] +
				__gamma[i] * ss[1 + (v3 << 1)];

			X = cvFloor(x);		Y = cvFloor(y);
			ixR1 = X << 2;	ixG1 = ixR1 + 1;	ixB1 = ixR1 + 2;
			p1 = (byte*)(imgdata + step*Y);
			fastt[k] = p1[ixB1];
			fastt[k + 1] = p1[ixG1];
			fastt[k + 2] = p1[ixR1];
		}
		return;
	}


	int off_g = (nchannel == 3) ? 1 : 0;
	int off_r = (nchannel == 3) ? 2 : 0;

	for (int i = 0, k = 0; i < __nPixels; i++, k += 3) {
		tri_idx = __pixTri[i];
		v1 = __triangles[tri_idx][0];
		v2 = __triangles[tri_idx][1];
		v3 = __triangles[tri_idx][2];

		x = __alpha[i] * ss[v1 << 1] + __belta[i] * ss[v2 << 1] +
			__gamma[i] * ss[v3 << 1];
		y = __alpha[i] * ss[1 + (v1 << 1)] + __belta[i] * ss[1 + (v2 << 1)] +
			__gamma[i] * ss[1 + (v3 << 1)];

#ifdef BINLINEAR
		X = cvFloor(x);	Y = cvFloor(y);	X1 = cvCeil(x);	Y1 = cvCeil(y);
		s0 = x - X;		t0 = y - Y;		s1 = 1 - s0;		t1 = 1 - t0;

		ixB1 = nchannel*X; ixG1 = ixB1 + off_g;	ixR1 = ixB1 + off_r;
		ixB2 = nchannel*X1;	ixG2 = ixB2 + off_g;	ixR2 = ixB2 + off_r;

		p1 = (byte*)(imgdata + step*Y);
		p2 = (byte*)(imgdata + step*Y1);
		fastt[k] = s1*(t1*p1[ixB1] + t0*p2[ixB1]) + s0*(t1*p1[ixB2] + t0*p2[ixB2]);
		fastt[k + 1] = s1*(t1*p1[ixG1] + t0*p2[ixG1]) + s0*(t1*p1[ixG2] + t0*p2[ixG2]);
		fastt[k + 2] = s1*(t1*p1[ixR1] + t0*p2[ixR1]) + s0*(t1*p1[ixR2] + t0*p2[ixR2]);

#else
		X = cvFloor(x);		Y = cvFloor(y);
		ixB1 = X*nchannel;	ixG1 = ixB1 + off_g;	ixR1 = ixB1 + off_r;
		p1 = (byte*)(imgdata + step*Y);
		fastt[k] = p1[ixB1];
		fastt[k + 1] = p1[ixG1];
		fastt[k + 2] = p1[ixR1];
#endif
	}
}

void piecewiseAffineWarpAAModel::writeStream(std::ofstream& os) {
	int i, j;

	os.write((char*)&__pointsCount, sizeof(int));
	os.write((char*)&__trianglesCount, sizeof(int));
	os.write((char*)&__nPixels, sizeof(int));
	os.write((char*)&__xmin, sizeof(int));
	os.write((char*)&__ymin, sizeof(int));
	os.write((char*)&__width, sizeof(int));
	os.write((char*)&__height, sizeof(int));

	for (i = 0; i < __trianglesCount; i++) {
		os.write((char*)&__triangles[i][0], sizeof(int));
		os.write((char*)&__triangles[i][1], sizeof(int));
		os.write((char*)&__triangles[i][2], sizeof(int));
	}

	for (i = 0; i < __vtri.size(); i++) {
		int ii = __vtri[i].size();
		os.write((char*)&ii, sizeof(int));
		for (j = 0; j < __vtri[i].size(); j++) {
			os.write((char*)&__vtri[i][j], sizeof(int));
		}
	}

	for (i = 0; i < __nPixels; i++)	os.write((char*)&__pixTri[i], sizeof(int));

	for (i = 0; i < __nPixels; i++)	os.write((char*)&__alpha[i], sizeof(double));

	for (i = 0; i < __nPixels; i++)	os.write((char*)&__belta[i], sizeof(double));

	for (i = 0; i < __nPixels; i++)	os.write((char*)&__gamma[i], sizeof(double));

	for (i = 0; i < __height; i++) {
		for (j = 0; j < __width; j++)
			os.write((char*)&__rect[i][j], sizeof(int));
	}

	__referenceshape.writeStream(os);
}

void piecewiseAffineWarpAAModel::readStream(std::ifstream& is) {
	int i, j;

	is >> __pointsCount >> __trianglesCount >> __nPixels >> __xmin >> __ymin >> __width >> __height;
	/* is.read((char*)&__pointsCount, sizeof(int));
	is.read((char*)&__trianglesCount, sizeof(int));
	is.read((char*)&__nPixels, sizeof(int));
	is.read((char*)&__xmin, sizeof(int));
	is.read((char*)&__ymin, sizeof(int));
	is.read((char*)&__width, sizeof(int));
	is.read((char*)&__height, sizeof(int)); */

	__triangles.resize(__trianglesCount);
	for (i = 0; i < __trianglesCount; i++) {
		__triangles[i].resize(3);
		is >> __triangles[i][0] >> __triangles[i][1] >> __triangles[i][2];
	}

	__vtri.resize(__pointsCount);
	for (i = 0; i < __pointsCount; i++) {
		int ii;
		is >> ii;
		__vtri[i].resize(ii);
		for (j = 0; j < ii; j++) is >> __vtri[i][j];
	}

	__pixTri.resize(__nPixels);
	for (i = 0; i < __nPixels; i++)	is >> __pixTri[i];
	__alpha.resize(__nPixels);
	for (i = 0; i < __nPixels; i++)	is >> __alpha[i];
	__belta.resize(__nPixels);
	for (i = 0; i < __nPixels; i++)	is >> __belta[i];

	__gamma.resize(__nPixels);
	for (i = 0; i < __nPixels; i++)	is >> __gamma[i];

	__rect.resize(__height);
	for (i = 0; i < __height; i++) {
		__rect[i].resize(__width);
		for (j = 0; j < __width; j++) is >> __rect[i][j];
	}
	__referenceshape.resize(__pointsCount);
	__referenceshape.readStream(is);
}
