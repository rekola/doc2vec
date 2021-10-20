#ifndef TAGGED_BROWN_CORPUS_H
#define TAGGED_BROWN_CORPUS_H

#include "common_define.h"

#include <string>
#include <vector>

class Doc2Vec;
//==================TaggedDocument============================
class TaggedDocument
{
public:
  TaggedDocument() { }
  TaggedDocument(const std::vector<std::string> & words) : m_words(words) { }

  void clear() {
    m_tag.clear();
    m_words.clear();
  }
  void addWord(const std::string & word) { m_words.push_back(word); }
  
  std::string m_tag;
  std::vector<std::string> m_words;
};
//==================TaggedBrownCorpus============================
class TaggedBrownCorpus
{
public:
  TaggedBrownCorpus(const std::string & train_file, long long seek = 0, long long limit_doc = -1);
  ~TaggedBrownCorpus();

  TaggedDocument * next();
  void rewind();
  long long getDocNum() const {return m_doc_num;}
  long long tell() {return ftell(m_fin);}
  void close() {fclose(m_fin);m_fin=NULL;}

private:
  int readWord(std::string & word);

  FILE* m_fin;
  TaggedDocument m_doc;
  long long m_seek;
  long long m_doc_num;
  long long m_limit_doc;
};

//==================UnWeightedDocument============================
class Doc2Vec;

class UnWeightedDocument
{
public:
  UnWeightedDocument() { }
  UnWeightedDocument(Doc2Vec * doc2vec, TaggedDocument * doc);

  void save(FILE * fout) const;
  void load(FILE * fin);

  std::vector<long long> m_words_idx;
};

//==================WeightedDocument============================
class WeightedDocument : public UnWeightedDocument
{
public:
  WeightedDocument(Doc2Vec * doc2vec, TaggedDocument * doc);

  std::vector<real> m_words_wei;
};

#endif
