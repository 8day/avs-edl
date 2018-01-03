/*
EDL log format
	frameX;clipY;frameXevnY;clipU;frameXevnU;clipV;frameXevnV;clipY;frameXoddY;clipU;frameXoddU;clipV;frameXoddV

	compared to more simple format where each field is on it's own line, this formatting simplifies processing
	when you need to do complex manipulations.


EDL supports only planar YUV formats, and what's more -- all clips must be in same color space.


how to increase maximum amount of supplied clips
	for this, simply add extra arguments to AviSynth function signiture, after what recompile this plugin.
	e.g., right after "[c9]c" insert "[cN]c[cN+1]c ... [cN+M]c".


initializer lists (VS2005 vs VS2017)
	Prior to Visual C++ 2005, the base class constructor was called before the initializer list when compiling
	with Managed Extensions for C++. Now, when compiling with /clr, the initializer list is called first.


TODO
	() detection of field-based or frame-based mode from clips;
	() after thinking it through, I think EDL should support frame-based mode: specification of clip:frame mapping
		**for all three planes**, as well as processing in frame-based way.
	() ATM there is some bug related to GetParity()...
		() first of all, the solution is to apply AssumeFieldBased()/AssumeTFF()/AssumeBFF() to first clip, which
			BTW is used to construct VideoInfo of returned clip;
		() the issue happens when we call SelectEvery(1, 0), which seems to change "vi.num_frames" as EDL itself,
			which leads to multiple edits of same variable;
		() after inspecting the trace log from STDERR it seem that AvsPmod is the one causing it because error happens
			in line #232 in pyavs.py: "self.GetParity = self.clip.get_parity(0)" -- baked in value of the parity!
			this can be proved by abscence of errors when same script is fed to VirtualDub;
		() in case of diffing "Subtract(SelectEvery(1, -1), SelectEvery(1, 0))" the error can be avoided by using shorthand
			version "Subtract(SelectEvery(1, -1))" with implicit or explicit "last", which is same as "SelectEvery(1, 0)";
	() caught some weird bug that when you navigate directly to some frame you get combed frame, while after reloading and
		navigating to that frame from other place makes combing disappear...
*/


#include <windows.h>
#include "avisynth.h"

#include <iostream>
#include <fstream>

#include <vector>

#include <boost/array.hpp>
#include <boost/regex.hpp> //some boost version has issues with initializer lists and won't compile (either 1.59.0 or 1.60.0).
#include <boost/lexical_cast.hpp> //for the alternative to stoi().


using namespace std;


//VS2005 does not support C99 standard with round().
inline double round_c99(double x) { return floor(x + 0.5); }


class EDL : public IClip {
public:
	PVideoFrame __stdcall GetFrame(int n, IScriptEnvironment* env);
	void __stdcall GetAudio(void* buf, __int64 start, __int64 count, IScriptEnvironment* env) {
		child->GetAudio(buf, start, count, env);
	};
	const VideoInfo& __stdcall GetVideoInfo() {
		return vi;
	};
	bool __stdcall GetParity(int n) {
		return child->GetParity(n);
	};
	int __stdcall SetCacheHints(int cachehints, int frame_range) {
		return 0;
	};
	EDL(AVSValue args, IScriptEnvironment* env);
	~EDL();
private:
	PClip child;
	VideoInfo vi;

	boost::array<int, 3> planes;

	ifstream edl_file;
	vector<boost::array<int, 12>> mappings; //vector-of-arrays should be efficient enough to avoid overcomplicating the system with pure array.
	vector<PClip> clips;
};


EDL::EDL(AVSValue args, IScriptEnvironment* env) {
	vector<VideoInfo> infos;
	vector<int> framebased_framecounts;


	//"initialize" supplied clips.
	//see http://avisynth.nl/index.php/Filter_SDK/Cplusplus_API#AVSValue for extra info.
	int num_clips = 0;
	for (int i = 0; i < args.ArraySize() - 2; i++) {
		if (args[i].Defined()) {
			PClip curr_clip = args[i].AsClip();
			VideoInfo curr_vi = curr_clip->GetVideoInfo();
			clips.push_back(curr_clip);
			infos.push_back(curr_vi);
			framebased_framecounts.push_back(int(round_c99(curr_vi.num_frames / 2)));
			num_clips += 1;
		};
	};


	//check supplied path and file it points to.
	const char* path_to_edl = args[args.ArraySize() - 2].AsString("edl.txt");
	edl_file.open(path_to_edl);
	if (!edl_file.good()) env->ThrowError("EDL: couldn't open edit decision list.");


	//set limit of lines in log.
	int line_limit = args[args.ArraySize() - 1].AsInt(100000);


	//check if at least one clip have been provided.
	//it could've been done in filter definition,
	//but when done explicitly it is less obscure.
	if (num_clips == 0) env->ThrowError("EDL: no clips have been provided.");


	//verify video info in provided clips.
	const VideoInfo& first_clip_vi = infos[0];
	if (!first_clip_vi.IsPlanar() || !first_clip_vi.IsYUV()) env->ThrowError("EDL: clip #%d must be in planar YUV!");
	for (int i = 0; i < num_clips; i++) {
		if (!infos[i].IsSameColorspace(first_clip_vi)) env->ThrowError("EDL: clip #%d must be in same color space as #0!", i);
		if (!infos[i].IsFieldBased()) env->ThrowError("EDL: clip #%d must be field-based!", i);
	};


	//parse log.
	//this part uses C++ "string" as it is more handy in this case,
	//also because it won't be used anywhere else.
	int num_lines = 0; //needed to set timeline length/num_frames in VideoInfo.
	const boost::regex re("^(\\d+?);(\\d+?);(\\d+?);(\\d+?);(\\d+?);(\\d+?);(\\d+?);(\\d+?);(\\d+?);(\\d+?);(\\d+?);(\\d+?)$");
	boost::smatch results;
	string line = "";
	string sub_line = "";
	while (getline(edl_file, line)) {
		num_lines += 1;
		if (num_lines > line_limit) {
			env->ThrowError("EDL: amount of lines in provided log is above \"line_limit\".");
		} else if (boost::regex_search(line, results, re)) {
			boost::array<int, 12> frame_map;
			for (int i = 0; i < 12; i++) {
				//VS2005 does not support C++11 stoi(), hence boost::lexical_cast(str).
				//we don't have to check if operation was successful because validity
				//of string was checked by RE.
				sub_line = results[i+1].str();
				frame_map[i] = boost::lexical_cast<int>(sub_line);
			};
			mappings.push_back(frame_map);
		} else {
			env->ThrowError("EDL: wrong formatting in line #%d.", num_lines);
		};
	};


	//update clip properties.
	child = clips[0]; //use misc data, e.g. audio, from first clip.
	vi = infos[0]; //construct VideoInfo because it is used in "dst = env->NewVideoFrame(vi)".
	vi.num_frames = num_lines * 2; //update VideoInfo's num_frames for resulting clip.  x2 to account for frame-based log, while clips will be field-based.  this also helps to make clip produced by EDL not tied to framecount of one of input clips.


	//verify data from log.
	int curr_clip_idx;
	int curr_frame_num;
	for (int i = 0; i < int(mappings.size()); i++) {
		boost::array<int, 12>& frame_map = mappings[i];
		for (int j = 0; j < 6; j++) {
			curr_clip_idx = frame_map[j * 2];
			curr_frame_num = frame_map[j * 2 + 1];
			//check if clip index is within total amount of provided clips.
			if (!(0 <= curr_clip_idx && curr_clip_idx < num_clips)) env->ThrowError("EDL: clip #%d is outside of allowed range [0;%d).", curr_clip_idx, num_clips);
			//check if frame is within clip timeline.
			if (!(0 <= curr_frame_num && curr_frame_num < framebased_framecounts[curr_clip_idx])) env->ThrowError("EDL: frame #%d is outside of clip #%d timeline, which is [0;%d).", curr_frame_num, curr_clip_idx, framebased_framecounts[curr_clip_idx]);
		};
	};


	//shared variable.
	//initializer list would've been better,
	//esp. if placed in class declaration,
	//but it is not supported until ~VS2015.
	planes[0] = PLANAR_Y;
	planes[1] = PLANAR_U;
	planes[2] = PLANAR_V;
};


EDL::~EDL() {
	if (edl_file.is_open()) edl_file.close();
};


PVideoFrame __stdcall EDL::GetFrame(int n, IScriptEnvironment* env) {
	PVideoFrame dst = env->NewVideoFrame(vi);

	int prt = (n % 2 == 0) ? 0 : 1;
	boost::array<int, 12>& frame_map = mappings[int(round_c99(n / 2))];

	//WARNING: aligned planes mess things up.
	for (int pln = 0; pln < 3; pln++) {
		PVideoFrame src = clips[frame_map[prt * 6 + pln * 2]]->GetFrame(frame_map[prt * 6 + pln * 2 + 1] * 2 + prt, env);

		const unsigned char* srcp = src->GetReadPtr(planes[pln]);
		const int src_pitch = src->GetPitch(planes[pln]);
		const int src_row_size = src->GetRowSize(planes[pln]);
		const int src_height = src->GetHeight(planes[pln]);

		unsigned char* dstp = dst->GetWritePtr(planes[pln]);
		const int dst_pitch = dst->GetPitch(planes[pln]);

		env->BitBlt(dstp, dst_pitch, srcp, src_pitch, src_row_size, src_height);
	};

	return dst;
};


AVSValue __cdecl Create_EDL(AVSValue args, void* user_data, IScriptEnvironment* env) {
	return new EDL(args, env);
};


const AVS_Linkage *AVS_linkage = 0;


extern "C" __declspec(dllexport) const char* __stdcall AvisynthPluginInit3(IScriptEnvironment* env, const AVS_Linkage* const vectors) {
	AVS_linkage = vectors;
	env->AddFunction("EDL", "[c0]c[c1]c[c2]c[c3]c[c4]c[c5]c[c6]c[c7]c[c8]c[path_to_edl]s[line_limit]i", Create_EDL, 0);
	return "EDL plugin";
};
