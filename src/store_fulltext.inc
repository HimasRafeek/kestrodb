// Included inside class Store (kestro.hpp): FULLTEXT indexes and SQL MATCH ... AGAINST. See
// SQL.md; the tokenizer, query syntax and BM25 are in fulltext.hpp.
//
// A FULLTEXT index is a secondary index (SecIndex, fulltext set) on one or more columns of a
// table, kept in the same kind of ordered set, built lazily from the rows like the others
// and changed by every commit (index_deltas, apply_catalog): nothing of it is stored, so it
// is right after reopen, compaction, crashes and replication. Each row with tokens has
//   term + 0x00 + id      once per distinct term of its text values (id: the row key
//                         without "table\0"), the postings, in term then row order;
//   0x00 + id + n         its token count n (4 bytes, big-endian), for BM25's lengths.
// The index holds no positions or counts: MATCH reads the row's text again to decide whether
// it matches and to score it, so the index only has to find every row that can match (the
// postings of the query's words) and give the statistics (documents, lengths, document
// frequencies). Rows a transaction changed are matched from their own text the same way.
private:
 static uint32_t marker_tokens(std::string_view e){uint32_t n=0;for(size_t i=e.size()-4;i<e.size();i++)n=(n<<8)|uint8_t(e[i]);return n;}
 // The entries of a row (sorted: the token count first), none when its text has no tokens.
 static void fulltext_entries(const SecIndex&ix,std::string_view doc,std::string_view key,std::vector<std::string>&out){
  out.clear();std::string_view id=key.substr(ix.collection.size()+1);size_t n=0;
  for(const auto&p:ix.cpaths){auto v=json::field(doc,p);if(v.tag!=json::STRING)continue;fts::tokens(v.str,ix.min_len,[&](std::string_view t){out.emplace_back(t);n++;});}
  if(!n)return;
  std::sort(out.begin(),out.end());out.erase(std::unique(out.begin(),out.end()),out.end());
  for(auto&t:out){t+='\0';t.append(id.data(),id.size());}  // still sorted: a term that is a prefix of another sorts first either way
  std::string m(1,'\0');m.append(id.data(),id.size());uint32_t c=uint32_t(std::min<size_t>(n,UINT32_MAX));for(int s=24;s>=0;s-=8)m+=char((c>>s)&0xFF);
  out.insert(out.begin(),std::move(m));
 }
 // A commit's change to an index's statistics (readers_ exclusive).
 static void fulltext_count(SecIndex&ix,const std::string&remove,const std::string&add){
  if(!remove.empty()&&remove[0]=='\0'){if(ix.ft_docs)ix.ft_docs--;ix.ft_tokens-=std::min<uint64_t>(ix.ft_tokens,marker_tokens(remove));}
  if(!add.empty()&&add[0]=='\0'){ix.ft_docs++;ix.ft_tokens+=marker_tokens(add);}
 }
 // Row ids (sorted) with word w, or with a term starting with w.
 static void fulltext_postings(const SecIndex&ix,const std::string&w,bool prefix,std::vector<std::string>&out){
  std::string lo=w,hi=prefix?json::prefix_end(w):w+'\x01';if(!prefix)lo+='\0';
  for(auto p=ix.set.seek(lo);ix.set.valid(p);p=ix.set.next(p)){
   auto e=ix.set.get(p);if(!hi.empty()&&e>=hi)break;
   check_deadline();out.emplace_back(e.substr(e.find('\0')+1));
  }
  if(prefix){std::sort(out.begin(),out.end());out.erase(std::unique(out.begin(),out.end()),out.end());}
 }
 static std::vector<std::string> intersect_ids(const std::vector<std::string>&a,const std::vector<std::string>&b){std::vector<std::string> o;std::set_intersection(a.begin(),a.end(),b.begin(),b.end(),std::back_inserter(o));return o;}
 // Statistics and candidate rows of a query (readers_ shared or the writer lock held).
 void fulltext_stats(const SecIndex&ix,fts::Prepared&p){
  const auto&items=p.query.items;
  p.stats.docs=double(ix.ft_docs);p.stats.avgdl=ix.ft_docs?double(ix.ft_tokens)/double(ix.ft_docs):1;
  std::vector<std::vector<std::string>> sets(items.size());
  for(size_t i=0;i<items.size();i++){
   const auto&it=items[i];
   // A phrase: rows with all its words (the row's text decides whether they are in a row).
   for(size_t w=0;w<it.words.size();w++){
    std::vector<std::string> ids;fulltext_postings(ix,it.words[w],it.prefix&&w+1==it.words.size(),ids);
    sets[i]=w?intersect_ids(sets[i],ids):std::move(ids);
    if(sets[i].empty())break;
   }
   p.stats.df.push_back(double(sets[i].size()));
  }
  // Rows that can match: those with every required item; without one, those with any
  // optional item (excluded items are left to the row's own check).
  std::vector<std::string> ids;bool required=false;
  for(size_t i=0;i<items.size();i++)if(items[i].kind==fts::Item::Required){ids=required?intersect_ids(ids,sets[i]):sets[i];required=true;}
  if(!required){
   for(size_t i=0;i<items.size();i++)if(items[i].kind==fts::Item::Optional)ids.insert(ids.end(),sets[i].begin(),sets[i].end());
   std::sort(ids.begin(),ids.end());ids.erase(std::unique(ids.begin(),ids.end()),ids.end());
  }
  auto keys=std::make_shared<std::vector<std::string>>();keys->reserve(ids.size());
  const std::string prefix=ix.collection+'\0';for(const auto&id:ids)keys->push_back(prefix+id);
  // A snapshot older than the index (BEGIN CONCURRENT): rows changed since may have matched
  // then; the scan reads them as of the snapshot and checks them.
  if(Snap*sn=tl_snap();sn&&sn->store==this&&sn->registered){
   size_t before=keys->size();
   {std::lock_guard<std::mutex>l(undo_m_);for(const auto&[at,k]:undo_log_)if(at>=sn->end&&k->compare(0,prefix.size(),prefix)==0)keys->push_back(*k);}
   if(keys->size()!=before){std::sort(keys->begin(),keys->end());keys->erase(std::unique(keys->begin(),keys->end()),keys->end());}
  }
  p.keys=std::move(keys);
 }
 // The MATCH ... AGAINST of a statement, found by node while it runs (sql::fulltext_hook).
 struct FtsScope:sql::FulltextHook{
  const sql::FulltextHook*prev=sql::fulltext_hook();bool set=false;
  std::vector<std::pair<const sql::Expr*,fts::Prepared>> list;
  const fts::Prepared*find(const sql::Expr&e)const override{for(const auto&x:list)if(x.first==&e)return &x.second;return prev?prev->find(e):nullptr;}
  FtsScope()=default;FtsScope(const FtsScope&)=delete;FtsScope&operator=(const FtsScope&)=delete;
  ~FtsScope(){if(set)sql::fulltext_hook()=prev;}
 };
 static void match_nodes(const sql::Expr&e,std::vector<const sql::Expr*>&out){
  if(sql::is_match(e))out.push_back(&e);
  for(const auto&a:e.args)match_nodes(a,out);
  if(e.over){for(const auto&p:e.over->partition)match_nodes(p,out);for(const auto&o:e.over->order)match_nodes(o.first,out);}
 }
 static bool refers_to_row(const sql::Expr&e){if(e.kind==sql::Expr::Col||e.sub)return true;for(const auto&a:e.args)if(refers_to_row(a))return true;return false;}
 static bool select_has_match(const sql::Select&s){
  for(const auto&it:s.items)if(!it.star&&sql::has_match(it.e))return true;
  if((s.where&&sql::has_match(*s.where))||(s.having&&sql::has_match(*s.having)))return true;
  for(const auto&g:s.group)if(sql::has_match(g))return true;
  for(const auto&o:s.order)if(sql::has_match(o.e))return true;
  for(const auto&f:s.from)if(f.on&&sql::has_match(*f.on))return true;
  return false;
 }
 // Prepares every MATCH of a one-table statement (its table prepared; readers_ shared or the
 // writer lock held): each needs a FULLTEXT index on exactly its columns (in any order) and a
 // constant query. The same query twice (WHERE and the select list) is looked up once.
 void fulltext_prepare(FtsScope&scope,const std::string&table,const std::vector<const sql::Expr*>&roots,const Params&params){
  std::vector<const sql::Expr*> nodes;for(const auto*r:roots)if(r)match_nodes(*r,nodes);
  if(nodes.empty())return;
  struct Seen {const SecIndex*ix;bool boolean,null;std::string text;size_t at;};std::vector<Seen> seen;
  for(const auto*e:nodes){
   std::vector<std::string> cols;std::string names;
   for(size_t k=0;k+1<e->args.size();k++){std::string n;for(size_t i=0;i<e->args[k].path.size();i++)n+=(i?".":"")+e->args[k].path[i];names+=(k?", ":"")+n;cols.push_back(std::move(n));}
   std::set<std::string> want(cols.begin(),cols.end());
   const SecIndex*ix=nullptr;for(const auto&x:sec_)if(x->fulltext&&x->collection==table&&std::set<std::string>(x->cols.begin(),x->cols.end())==want){ix=x.get();break;}
   if(!ix)throw std::runtime_error("INPUT SQL: MATCH ("+names+") needs a FULLTEXT index on exactly these columns of "+table+" (CREATE FULLTEXT INDEX ON "+table+" ("+names+"))");
   if(!ix->built){
    // A write statement prepares only unique indexes (first builds without the writer lock):
    // under the writer lock, its MATCH builds the index here.
    if(tl_writer_held()!=this)throw std::runtime_error("INPUT SQL: the FULLTEXT index "+ix->name+" is not built yet; run the statement again");
    if(tl_overlap())throw Deferred{};
    build_index_locked(*const_cast<SecIndex*>(ix));
   }
   const auto&qe=e->args.back();
   if(refers_to_row(qe))throw std::runtime_error("INPUT SQL: AGAINST takes a constant text (a string or a parameter), not columns or subqueries");
   sql::Row ctx{"{}",&params};auto v=sql::eval(qe,ctx);
   if(!sql::is_null(v)&&v.tag!=json::STRING)throw std::runtime_error("INPUT SQL: AGAINST needs text");
   bool null=sql::is_null(v);
   fts::Prepared p;p.table=table;p.min_len=ix->min_len;
   size_t k=0;while(k<seen.size()&&!(seen[k].ix==ix&&seen[k].boolean==e->boolean&&seen[k].null==null&&seen[k].text==v.str))k++;
   if(k<seen.size())p=scope.list[seen[k].at].second;
   else{if(!null)p.query=fts::parse(v.str,e->boolean,ix->min_len);fulltext_stats(*ix,p);}
   seen.push_back({ix,e->boolean,null,null?std::string():v.str,scope.list.size()});
   scope.list.emplace_back(e,std::move(p));
  }
  if(!scope.set){scope.set=true;sql::fulltext_hook()=&scope;}
 }
 void fulltext_prepare(FtsScope&scope,const sql::Select&s,const Params&params){
  std::vector<const sql::Expr*> roots;
  for(const auto&it:s.items)if(!it.star)roots.push_back(&it.e);
  roots.push_back(s.where?&*s.where:nullptr);roots.push_back(s.having?&*s.having:nullptr);
  for(const auto&g:s.group)roots.push_back(&g);
  for(const auto&o:s.order)roots.push_back(&o.e);
  fulltext_prepare(scope,s.table,roots,params);
 }
 // The rows a WHERE's MATCH allows (top-level AND terms: MATCH ..., MATCH ... > c with c >= 0,
 // MATCH ... >= c with c > 0; the fewest rows of several). Null: no narrowing.
 static std::shared_ptr<const std::vector<std::string>> fulltext_keys(const sql::Expr&e,const std::string&table,const Params&params){
  using E=sql::Expr;
  if(e.kind==E::And){
   auto a=fulltext_keys(e.args[0],table,params),b=fulltext_keys(e.args[1],table,params);
   if(!a)return b;if(!b)return a;return a->size()<=b->size()?a:b;
  }
  const E*m=nullptr;
  if(sql::is_match(e))m=&e;
  else if(e.kind==E::Bin&&(e.op==">"||e.op==">="||e.op=="<"||e.op=="<=")){
   bool left=sql::is_match(e.args[0]);const E&other=e.args[left?1:0];
   std::string op=e.op;if(!left){if(!sql::is_match(e.args[1]))return nullptr;op=op=="<"?">":op=="<="?">=":"";}
   if((op==">"||op==">=")&&(other.kind==E::Lit||other.kind==E::Param)){
    sql::Row ctx{"{}",&params};auto v=sql::eval(other,ctx);
    if(v.tag==json::NUMBER&&(op==">"?v.num>=0:v.num>0))m=&e.args[left?0:1];
   }
  }
  if(!m)return nullptr;
  const auto*p=sql::fulltext_hook()->find(*m);
  return p&&p->table==table?p->keys:nullptr;
 }
public:
 // FULLTEXT index on 1 to 8 columns (text values; other values are not indexed). Returns
 // false when one on the same columns (in any order) exists. Writer lock held.
 bool create_fulltext_locked(const std::string&collection,const std::vector<std::string>&cols,const std::string&name,size_t min_len){
  require(query::valid_collection(collection),"INPUT index collection");
  require(!cols.empty()&&cols.size()<=8,"LIMIT a FULLTEXT index covers 1 to 8 columns");
  require(min_len>=1&&min_len<=64,"INPUT min_token_length must be 1 to 64");
  std::string field;std::vector<std::vector<std::string>> paths;
  for(size_t i=0;i<cols.size();i++){
   require(cols[i]!="_key"&&cols[i]!="id","INPUT a FULLTEXT index covers text columns, not id");
   for(size_t k=0;k<i;k++)require(cols[k]!=cols[i],"INPUT the index lists "+cols[i]+" twice");
   paths.push_back(json::split_path(cols[i]));field+=(i?",":"")+cols[i];
  }
  {std::shared_lock<ReadWriteMutex>r(readers_);healthy();}
  materialize();load_catalog_locked();
  std::set<std::string> set(cols.begin(),cols.end());
  for(const auto&ix:sec_){
   if(ix->fulltext&&ix->collection==collection&&std::set<std::string>(ix->cols.begin(),ix->cols.end())==set)return false;
   require(name.empty()||ix->name!=name||ix->collection!=collection,"INPUT index name "+name+" is already used");
  }
  require(sec_.size()<256,"LIMIT at most 256 indexes");
  auto ix=std::make_unique<SecIndex>();ix->name=name.empty()?fulltext_index_name(collection,cols):name;ix->collection=collection;ix->field=field;ix->cols=cols;ix->cpaths=std::move(paths);ix->fulltext=true;ix->min_len=min_len;
  build_index_locked(*ix);
  write_catalog_locked(ix.get(),nullptr);
  std::unique_lock<ReadWriteMutex>r(readers_);sec_.push_back(std::move(ix));return true;
 }
 static std::string fulltext_index_name(const std::string&collection,const std::vector<std::string>&cols){std::string n=collection;for(const auto&c:cols)n+="_"+c;return n+"_fulltext";}
private:
 // An index as DDL makes it again (ALTER TABLE renames and drops): keeps its name unless it
 // had the automatic one.
 struct IndexDef {std::string name;std::vector<std::string> cols;bool unique=false,fulltext=false;size_t min_len=1;};
 static IndexDef index_def(const SecIndex&ix){return {ix.name,index_columns(ix),ix.unique,ix.fulltext,ix.min_len};}
 static std::string auto_index_name(const std::string&table,const IndexDef&d){return d.fulltext?fulltext_index_name(table,d.cols):default_index_name(table,d.cols);}
 void recreate_index_locked(const std::string&from_table,const std::string&table,const IndexDef&old,const std::vector<std::string>&cols){
  std::string name=old.name==auto_index_name(from_table,old)?std::string():old.name;
  if(old.fulltext)create_fulltext_locked(table,cols,name,old.min_len);
  else create_composite_locked(table,cols,old.unique,name);
 }
