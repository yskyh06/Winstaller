#include "core/update_protocol.hpp"
#include "core/hash_verifier.hpp"
#include "platform/update_service.hpp"
#include "platform/update_store.hpp"
#include "platform/wim_deployer.hpp"
#include "ui/winstaller_tui.hpp"
#include <sstream>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <unistd.h>
using namespace litewin;
using Json=nlohmann::json;
namespace fs=std::filesystem;
static void require(bool ok,const char* msg){if(!ok)throw std::runtime_error(msg);}
template<class F>static void rejects(F f){bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}require(rejected,"Invalid input was accepted");}
// Public RFC 8032 test vector, never a production signing key.
static const std::string seed="9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";
static const std::string pub="d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a";
static std::string b64(const std::string& bytes){std::string s(4*((bytes.size()+2)/3)+1,'\0');auto n=EVP_EncodeBlock(reinterpret_cast<unsigned char*>(s.data()),reinterpret_cast<const unsigned char*>(bytes.data()),bytes.size());s.resize(n);return s;}
static std::string signed_raw(const std::string& data) {
    unsigned char raw[32];for(int i=0;i<32;++i)raw[i]=std::stoul(seed.substr(2*i,2),nullptr,16);
    auto* key=EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519,nullptr,raw,32);auto* ctx=EVP_MD_CTX_new();
    require(key&&ctx&&EVP_DigestSignInit(ctx,nullptr,nullptr,nullptr,key)==1,"Fixture signing init");
    std::string sig(64,'\0');size_t len=sig.size();require(EVP_DigestSign(ctx,reinterpret_cast<unsigned char*>(sig.data()),&len,reinterpret_cast<const unsigned char*>(data.data()),data.size())==1,"Fixture signing");EVP_MD_CTX_free(ctx);EVP_PKEY_free(key);
    return Json{{"key_id","test"},{"payload_base64",b64(data)},{"signature_base64",b64(sig)}}.dump();
}
static std::string sign(const Json& j){return signed_raw(j.dump());}
static UpdateConfig config(){UpdateConfig c;c.keys={{"test",pub,{"catalog","release"}}};c.catalog_url="https://raw.githubusercontent.com/yskyh06/Winstaller/catalog/catalog-v1.signed.json";c.release_url="https://raw.githubusercontent.com/yskyh06/Winstaller/catalog/updates/stable-v1.signed.json";return c;}
static Json common(const char* type){auto now=std::time(nullptr);return {{"document_type",type},{"schema_version",1},{"revision",2},{"published_at",now-60},{"expires_at",now+3600}};}
static Json target(){return {{"id","win12-pro-ko"},{"product","win12-pro"},{"display_name","Windows future fixture"},{"language","ko-KR"},{"language_name","Korean"},{"family","windows"},{"install_strategy","windows-esd-uefi-v1"},{"architecture","x64"},{"version","test"},{"minimum_engine_api",1},{"requires_capabilities",{"uefi-x64","esd-apply"}},{"enabled",true},{"support_status","verified"},{"windows",{{"expected_image_name","Windows future Pro"}}},{"artifact",{{"url","https://dl.delivery.mp.microsoft.com/fixture.esd"},{"sha256",std::string(64,'a')},{"size_bytes",123456789},{"format","esd"}}}};}
static Json catalog(){auto j=common("catalog");j["min_catalog_api"]=1;j["targets"]=Json::array({target()});return j;}
static Json release(){auto j=common("release");j.update({{"release_id","test-2"},{"release_sequence",2},{"version","test.2"},{"channel","stable"},{"architecture","x64"},{"boot_abi","winstaller-slots-v1"},{"usb_layout_versions",{2}},{"minimum_updater_api",1},{"release_notes","Fixture release"}});j["components"]=Json::array();for(auto role:{"kernel","initramfs","rootfs"})j["components"].push_back({{"role",role},{"url",std::string("https://github.com/yskyh06/Winstaller/releases/download/test-2/")+role},{"sha256",std::string(64,'a')},{"size_bytes",16}});return j;}
struct Temp {
 fs::path root;Temp(){char p[]="/tmp/winstaller-update-test-XXXXXX";auto* d=mkdtemp(p);require(d,"tempdir");root=d;}
 ~Temp(){fs::remove_all(root);}
};
static void write(const fs::path& p,const std::string& body){fs::create_directories(p.parent_path());std::ofstream(p,std::ios::binary)<<body;}
static void factory(const fs::path& root) {
 auto p=root/"winstaller/slots/A";Json components=Json::object();
 for(auto role:{"kernel","initramfs","rootfs"}){auto file=p/update_component_path(role);write(file,"old-"+std::string(role));components[role]={{"size_bytes",fs::file_size(file)},{"sha256",compute_file_sha256(file)}};}
 write(p/"manifest.json",Json{{"slot","A"},{"release_id","factory"},{"generation",1},{"components",components}}.dump());
 std::string env="# GRUB Environment Block\nwinstaller_generation=1\n";env.resize(1024,'#');write(p/"ready.env",env);write(p/"ready.sha256",compute_file_sha256(p/"ready.env")+"  ready.env\n");write(p/"boot-success","factory\n1\n");
}
static AppRelease local_release(const fs::path& root){auto r=verify_release(sign(release()),config(),std::time(nullptr));for(auto& c:r.components){auto path=root/("payload-"+c.role);write(path,"new-"+c.role);c.size=fs::file_size(path);c.sha256=compute_file_sha256(path);}return r;}
static ComponentFetch local_fetch(const fs::path& root,int fail_after=100){return [root,fail_after,count=0](const UpdateComponent& c,int fd,std::stop_token,const UpdateProgress& progress) mutable {if(++count>fail_after)throw std::runtime_error("simulated power cut");std::ifstream f(root/("payload-"+c.role));std::string data((std::istreambuf_iterator<char>(f)),{});require(::write(fd,data.data(),data.size())==static_cast<ssize_t>(data.size()),"fixture write");if(progress)progress(data.size(),data.size());};}
class InputTerminal:public AnsiTerminal {
public:
 std::function<KeyEvent(int)> next;
 KeyEvent read_key(int=-1)override{return next?next(0):KeyEvent{Key::Escape};}
};
static void test(const std::string& name) {
 if(name=="ui_late_catalog") {
  auto c=config();auto cat=sign(catalog()),rel=sign(release());std::atomic<bool> proceed=false;
  UpdateService service(c,[&](const auto& url,std::stop_token stop){while(!proceed&&!stop.stop_requested())std::this_thread::sleep_for(std::chrono::milliseconds(1));return url==c.catalog_url?cat:rel;});service.check();
  InputTerminal term;WinstallerTui ui(term);ui.configure_updates(&service);int step=0;
  term.next=[&](int){switch(step++){case 0:proceed=true;return KeyEvent{Key::Down};case 1:for(int i=0;i<200&&service.snapshot().checking;++i)std::this_thread::sleep_for(std::chrono::milliseconds(5));return KeyEvent{Key::Down};case 2:return KeyEvent{Key::Enter};default:return KeyEvent{Key::Escape};}};
  std::ostringstream frame;auto* old=std::cout.rdbuf(frame.rdbuf());auto first=ui.prompt_edition_selection("",false);
  require(!first.second.remote_target,"Late catalog changed the user's selection");step=0;
  auto second=ui.prompt_edition_selection("",false);std::cout.rdbuf(old);
  require(second.second.remote_target&&second.second.remote_target->id=="win12-pro-ko","Pending catalog not applied on reentry");
 } else if(name=="catalog_linux") {
  auto j=catalog();auto& t=j["targets"][0];t["id"]="ubuntu-test";t["product"]="ubuntu-test";t["family"]="ubuntu";t["install_strategy"]="ubuntu-iso-uefi-v1";
  t["artifact"]["url"]="https://releases.ubuntu.com/test.iso";t["artifact"]["format"]="iso";t["linux"]={{"extractor","layered-squashfs"},{"image_layers",{"casper/minimal.squashfs","casper/desktop.squashfs"}}};
  auto c=verify_catalog(sign(j),config(),std::time(nullptr));require(!c.targets.front().windows&&c.targets.front().linux_target.image_layers.size()==2,"Linux layers lost");
  t["linux"]["image_layers"]={"../etc/shadow"};rejects([&]{verify_catalog(sign(j),config(),std::time(nullptr));});
 } else if(name=="ui_catalog") {
  auto c=config();auto cat=sign(catalog()),rel=sign(release());
  UpdateService service(c,[&](const auto& url,std::stop_token){return url==c.catalog_url?cat:rel;});service.check();
  for(int i=0;i<200&&service.snapshot().checking;++i)std::this_thread::sleep_for(std::chrono::milliseconds(5));
  InputTerminal term;WinstallerTui ui(term);ui.configure_updates(&service);
  int step=0;term.next=[&](int){switch(step++){case 0:return KeyEvent{Key::Down};case 1:return KeyEvent{Key::Down};case 2:return KeyEvent{Key::Enter};default:return KeyEvent{Key::Escape};}};
  std::ostringstream frame;auto* old=std::cout.rdbuf(frame.rdbuf());
  auto result=ui.prompt_edition_selection("",false);std::cout.rdbuf(old);
  require(result.first==ScreenAction::Next&&result.second.remote_target&&result.second.remote_target->id=="win12-pro-ko"&&result.second.catalog_revision==2,"Remote choice not frozen");
  require(frame.str().find("Update available")!=std::string::npos,"No optional update banner");
  term.next=[](int){return KeyEvent{Key::Escape};};frame.str("");old=std::cout.rdbuf(frame.rdbuf());require(!ui.show_update_modal(),"Later requested reboot");std::cout.rdbuf(old);
  require(frame.str().find("Optional Update")!=std::string::npos,"No details screen");
 } else if(name=="image_selection") {
  auto images=WimDeployer::parse_info_output("Index: 1\nName: Future Pro\nArchitecture: x86_64\nInstallation Type: Client\nIndex: 2\nName: Future Pro N\nArchitecture: x86_64\n");
  require(WimDeployer::resolve_exact_image(images,"Future Pro")->index==1,"Exact image index ignored");
  require(!WimDeployer::resolve_exact_image(images,"Missing"),"Missing edition fell back");
  images.front().architecture="x86";require(!WimDeployer::resolve_exact_image(images,"Future Pro"),"Wrong architecture accepted");
  images.front().architecture="x86_64";images.push_back(images.front());require(!WimDeployer::resolve_exact_image(images,"Future Pro"),"Ambiguous edition accepted");
 } else if(name=="signatures") {
  auto raw=sign(catalog());auto result=verify_catalog(raw,config(),std::time(nullptr));require(result.targets.size()==1&&result.find("win12-pro","ko-KR")->image_name=="Windows future Pro","Future ID not retained");
  auto e=Json::parse(raw);e["payload_base64"]=b64(catalog().dump()+" ");rejects([&]{verify_catalog(e.dump(),config(),std::time(nullptr));});
  auto c=config();c.keys.front().roles={"release"};rejects([&]{verify_catalog(raw,c,std::time(nullptr));});
  rejects([&]{verify_catalog(raw,UpdateConfig{},std::time(nullptr));});
  rejects([&]{verify_catalog(signed_raw("{\"document_type\":\"catalog\",\"document_type\":\"catalog\"}"),config(),std::time(nullptr));});
 } else if(name=="catalog_validation") {
  for(auto mutate:std::vector<std::function<void(Json&)>>{
   [](auto& j){j["targets"][0]["artifact"]["sha256"]="";},[](auto& j){j["targets"][0]["install_strategy"]="unknown";},
   [](auto& j){j["targets"][0]["enabled"]=true;j["targets"][0]["support_status"]="unverified";},
   [](auto& j){j["targets"].push_back(j["targets"][0]);},[](auto& j){j["targets"][0]["display_name"]="unsafe\033[2J";},
   [](auto& j){j["targets"][0]["artifact"]["url"]="https://localhost/file";},[](auto& j){j["targets"][0]["artifact"]["size_bytes"]=-1;},
   [](auto& j){j["expires_at"]=1;},[](auto& j){j["min_catalog_api"]=2;}}){auto j=catalog();mutate(j);rejects([&]{verify_catalog(sign(j),config(),std::time(nullptr));});}
  auto j=catalog();j["targets"][0]["minimum_engine_api"]=2;require(!verify_catalog(sign(j),config(),std::time(nullptr)).targets.front().supported,"Newer API installed");
  j["targets"]=Json::array();require(verify_catalog(sign(j),config(),std::time(nullptr)).targets.empty(),"Empty catalog resurrected old entries");
 } else if(name=="urls") {
  for(auto url:{"http://github.com/file","file:///tmp/x","https://github.com@127.0.0.1/x","https://github.com.evil.test/x","https://github.com:444/x","https://github.com/x#fragment","https://github.com/\nfile","https://user:secret@github.com/x"})require(!valid_update_url(url,false),"Unsafe endpoint accepted");
  require(valid_update_url("https://raw.githubusercontent.com/yskyh06/Winstaller/catalog/a.json",false),"Repo URL denied");
  require(valid_update_url("https://releases.ubuntu.com/fixture.iso",true),"Ubuntu URL denied");require(!valid_update_url("https://github.com/image.iso",true),"Nonofficial image source accepted");
 } else if(name=="release_validation") {
  auto j=release();require(verify_release(sign(j),config(),std::time(nullptr)).total_bytes()==48,"Size sum");j["components"].erase(0);rejects([&]{verify_release(sign(j),config(),std::time(nullptr));});
  j=release();j["components"][0]["size_bytes"]=4294967296ULL;rejects([&]{verify_release(sign(j),config(),std::time(nullptr));});
  j=release();j["components"][0]["url"]="https://github.com/yskyh06/Winstaller/releases/download/another-release/kernel";rejects([&]{verify_release(sign(j),config(),std::time(nullptr));});
  j=release();j["usb_layout_versions"]={2.5};rejects([&]{verify_release(sign(j),config(),std::time(nullptr));});
  j=release();j["boot_abi"]="future";require(!verify_release(sign(j),config(),std::time(nullptr)).compatible,"Unknown ABI accepted");
 } else if(name=="service") {
  auto c=config();auto cat=sign(catalog()),rel=sign(release());std::atomic<int> calls=0;
  UpdateService service(c,[&](const auto& url,std::stop_token){++calls;return url==c.catalog_url?cat:rel;});service.check();
  for(int i=0;i<200&&service.snapshot().checking;++i)std::this_thread::sleep_for(std::chrono::milliseconds(5));
  auto first=service.snapshot();require(first.catalog&&first.release,"Background metadata missing");
  auto j=catalog();j["revision"]=1;j["targets"]=Json::array();cat=sign(j);service.check();
  for(int i=0;i<200&&service.snapshot().checking;++i)std::this_thread::sleep_for(std::chrono::milliseconds(5));
  require(service.snapshot().catalog==first.catalog,"Catalog rolled back");
  auto renewed=release();renewed["revision"]=3;renewed["expires_at"]=std::time(nullptr)+7200;rel=sign(renewed);service.check();
  for(int i=0;i<200&&service.snapshot().checking;++i)std::this_thread::sleep_for(std::chrono::milliseconds(5));
  require(service.snapshot().release&&service.snapshot().release->revision==3,"Release freshness renewal failed");first.release=service.snapshot().release;
  require(!service.begin_update("unreviewed-id")&&service.snapshot().state==UpdateState::Failed,"Unreviewed release started");
  auto changed=release();changed["revision"]=4;changed["release_sequence"]=3;rel=sign(changed);service.check();
  for(int i=0;i<200&&service.snapshot().checking;++i)std::this_thread::sleep_for(std::chrono::milliseconds(5));
  require(service.snapshot().release==first.release&&!service.snapshot().check_error.empty(),"A release ID was reused with different metadata");
  cat="bad signature";service.check();for(int i=0;i<200&&service.snapshot().checking;++i)std::this_thread::sleep_for(std::chrono::milliseconds(5));
  require(service.snapshot().catalog==first.catalog&&!service.snapshot().check_error.empty(),"Last good catalog lost");
  auto short_catalog=catalog();short_catalog["expires_at"]=std::time(nullptr)+2;auto short_raw=sign(short_catalog);
  UpdateService expiring(c,[&](const auto& url,std::stop_token){return url==c.catalog_url?short_raw:sign(release());});expiring.check();
  for(int i=0;i<200&&expiring.snapshot().checking;++i)std::this_thread::sleep_for(std::chrono::milliseconds(5));
  require(expiring.snapshot().catalog!=nullptr,"Expiring fixture was not loaded");std::this_thread::sleep_for(std::chrono::milliseconds(2100));
  require(!expiring.snapshot().catalog,"Expired RAM catalog remained selectable");
  UpdateService disabled(UpdateConfig{},[&](const auto&,auto){++calls;return "";});auto count=calls.load();disabled.check();require(calls==count,"Unsigned network check started");
 } else {
  Temp temp;factory(temp.root);auto release=local_release(temp.root);auto active_hash=compute_file_sha256(temp.root/"winstaller/slots/A/arch/x86_64/airootfs.sfs");
  if(name=="slots_commit") {
   stage_update(temp.root,"A",release,{},local_fetch(temp.root),{});auto b=read_update_slot(temp.root,"B");require(b.generation==2&&b.release==release.id,"Slot not committed");
   rejects([&]{stage_update(temp.root,"A",release,{},local_fetch(temp.root),{});});
   rejects([&]{stage_update(temp.root,"B",release,{},local_fetch(temp.root),{});});
   acknowledge_update_slot(temp.root,"B");release.id="test-3";stage_update(temp.root,"B",release,{},local_fetch(temp.root),{});require(read_update_slot(temp.root,"A").generation==3,"Second update failed");return;
  } else if(name=="slots_power_cut") {
   for(int after=0;after<3;++after){Temp cut;factory(cut.root);auto r=local_release(cut.root);rejects([&]{stage_update(cut.root,"A",r,{},local_fetch(cut.root,after),{});});require(read_update_slot(cut.root,"B").generation==0&&read_update_slot(cut.root,"A").generation==1,"Incomplete slot committed");}
   rejects([&]{stage_update(temp.root,"A",release,{},local_fetch(temp.root),{},[]{throw std::runtime_error("cut before commit");});});
  } else if(name=="slots_resume") {
   rejects([&]{stage_update(temp.root,"A",release,{},[](const auto&,int fd,auto,const auto&){require(::write(fd,"ne",2)==2,"partial write");throw std::runtime_error("interrupted");},{});});
   bool resumed=false;
   stage_update(temp.root,"A",release,{},[&](const UpdateComponent& c,int fd,auto,const auto&){
       auto offset=lseek(fd,0,SEEK_END);if(offset==2)resumed=true;
       std::ifstream in(temp.root/("payload-"+c.role));in.seekg(offset);std::string body((std::istreambuf_iterator<char>(in)),{});
       require(::write(fd,body.data(),body.size())==static_cast<ssize_t>(body.size()),"resume write");
   },{});
   require(resumed&&read_update_slot(temp.root,"B").generation==2,"Partial download was not resumed");
  } else if(name=="slots_checksum") {
   release.components[2].sha256=std::string(64,'0');rejects([&]{stage_update(temp.root,"A",release,{},local_fetch(temp.root),{});});require(read_update_slot(temp.root,"B").generation==0,"Corrupt slot committed");
  } else if(name=="slots_cancel") {
   std::stop_source stop;stop.request_stop();rejects([&]{stage_update(temp.root,"A",release,stop.get_token(),local_fetch(temp.root),{});});
  } else if(name=="slots_symlink") {
   fs::create_directory_symlink(temp.root,temp.root/"winstaller/slots/B");rejects([&]{stage_update(temp.root,"A",release,{},local_fetch(temp.root),{});});
  } else throw std::runtime_error("Unknown test");
  require(read_update_slot(temp.root,"A").generation==1,"Active slot lost");require(compute_file_sha256(temp.root/"winstaller/slots/A/arch/x86_64/airootfs.sfs")==active_hash,"Active rootfs changed");
 }
}
int main(int argc,char** argv){try{if(argc==3&&std::string(argv[1])=="assets") {
    fs::path root=argv[2];auto r=release();r["release_id"]="lab-2";r["version"]="Lab 2";r["release_notes"]="Local signed update verification - test fixture only";
    for(auto& c:r["components"]){c["url"]="https://github.com/yskyh06/Winstaller/releases/download/lab-2/"+c["role"].get<std::string>();auto path=root/("payload-"+c["role"].get<std::string>());c["sha256"]=compute_file_sha256(path);c["size_bytes"]=fs::file_size(path);}
    write(root/"release.signed.json",sign(r));write(root/"catalog.signed.json",sign(catalog()));auto c=config();
    write(root/"config.json",Json{{"catalog_url",c.catalog_url},{"release_url",c.release_url},{"release_sequence",1},{"keys",Json::array({{{"id","test"},{"public_hex",pub},{"roles",{"catalog","release"}}}})}}.dump());return 0;
  }require(argc==2,"scenario missing");test(argv[1]);return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
