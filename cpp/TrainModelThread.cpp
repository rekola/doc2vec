#include "TrainModelThread.h"
#include "Model.h"
#include "TaggedBrownCorpus.h"
#include "Vocabulary.h"
#include "NN.h"

#include <cmath>

using namespace doc2vec;

TrainModelThread::TrainModelThread(long long id, Model * doc2vec,
				   std::unique_ptr<TaggedBrownCorpus> sub_corpus, bool infer)
  : m_id(id), m_doc2vec(doc2vec), m_corpus(std::move(sub_corpus)), m_infer(infer)
{
  m_start = clock();
  m_next_random = id;
  m_word_count = 0;
  m_last_word_count = 0;

  m_neu1 = std::unique_ptr<real[]>(new real[doc2vec->nn().dim()]);
  m_neu1e = std::unique_ptr<real[]>(new real[doc2vec->nn().dim()]);
}

void TrainModelThread::train()
{
  TaggedDocument * doc = NULL;
  for(int local_iter = 0; local_iter < m_doc2vec->iter(); local_iter++)
  {
    while((doc = m_corpus->next()) != NULL)
    {
      updateLR();
      buildDocument(*doc);
      if(!m_doc_vector) continue;
      trainDocument();
    }
    m_corpus->rewind();
    m_doc2vec->updateWordCountActual(m_word_count - m_last_word_count);
    m_word_count = 0;
    m_last_word_count = 0;
  }
}

void TrainModelThread::updateLR()
{
  long long train_words = m_doc2vec->wvocab().getTrainWords();
  if (m_word_count - m_last_word_count > 10000) { //statistics speed per 10000 words, and update learning rate
    m_doc2vec->updateWordCountActual(m_word_count - m_last_word_count);
    m_last_word_count = m_word_count;
    clock_t now = clock();
    fprintf(stderr, "%cAlpha: %f  Progress: %.2f%%  Words/sec: %.2fk  ", 13, m_doc2vec->getAlpha(),
	    m_doc2vec->m_word_count_actual / (real)(m_doc2vec->iter() * train_words + 1) * 100,
	    m_doc2vec->m_word_count_actual / ((real)(now - m_start + 1) / (real)CLOCKS_PER_SEC * 1000));
    fflush(stderr);
    m_doc2vec->setAlpha(m_doc2vec->m_start_alpha * (1 - m_doc2vec->m_word_count_actual / (real)(m_doc2vec->iter() * train_words + 1)));
    m_doc2vec->setAlpha(MAX(m_doc2vec->getAlpha(), m_doc2vec->getStartAlpha() * 0.0001));
  }
}

void TrainModelThread::buildDocument(TaggedDocument & doc, int skip)
{
  if(!m_infer) {
    m_doc_vector = nullptr;
    long long doc_idx = m_doc2vec->dvocab().searchVocab(doc.m_tag);
    if(doc_idx < 0) {
      return;
    }
    m_doc_vector = &(m_doc2vec->nn().get_dsyn0()[m_doc2vec->nn().dim() * doc_idx]);
  }
  m_sen.clear();
  m_sen_nosample.clear();
  auto & words = m_doc2vec->wvocab().getWords();
  for (size_t i = 0; i < doc.m_words.size(); i++) {
    if ((ssize_t)i != skip) {
      long long word_idx = m_doc2vec->wvocab().searchVocab(doc.m_words[i]);
      if (word_idx == -1) continue;
      if (word_idx == 0) break;
      m_word_count++;
      m_sen_nosample.push_back(word_idx);
      if (!down_sample(words[word_idx].cn)) {
	m_sen.push_back(word_idx);
      }
    }
  }
}

void TrainModelThread::trainSampleCbow(long long central, long long context_start, long long context_end)
{
  long long a, c, d, l2, last_word, target, label, cw = 0;
  long long central_word = m_sen[central];
  long long layer1_size = m_doc2vec->nn().dim();
  auto syn0 = m_doc2vec->nn().get_syn0();
  auto syn1 = m_doc2vec->nn().get_syn1();
  auto syn1neg = m_doc2vec->nn().get_syn1neg();
  auto & vocab = m_doc2vec->wvocab().getWords();

  for (c = 0; c < layer1_size; c++) m_neu1[c] = 0;
  for (c = 0; c < layer1_size; c++) m_neu1e[c] = 0;
  //averge context
  for(a = context_start; a < context_end; a++) if(a != central)
  {
    last_word = m_sen[a];
    for (c = 0; c < layer1_size; c++) m_neu1[c] += syn0[c + last_word * layer1_size];
    cw++;
  }
  for (c = 0; c < layer1_size; c++) m_neu1[c] += m_doc_vector[c];
  cw++;
  for (c = 0; c < layer1_size; c++) m_neu1[c] /= cw;
  //hierarchical softmax
  if (m_doc2vec->useHS()) {
    for (d = 0; d < vocab[central_word].codelen; d++) {
      real f = 0;
      l2 = vocab[central_word].point[d] * layer1_size;
      for (c = 0; c < layer1_size; c++) f += m_neu1[c] * syn1[c + l2];
      if (f <= -MAX_EXP) continue;
      else if (f >= MAX_EXP) continue;
      else f = m_doc2vec->m_expTable[(int)((f + MAX_EXP) * (EXP_TABLE_SIZE / MAX_EXP / 2))];
      real g = (1 - vocab[central_word].code[d] - f) * m_doc2vec->m_alpha;
      for (c = 0; c < layer1_size; c++) m_neu1e[c] += g * syn1[c + l2];
      if (!m_infer) for (c = 0; c < layer1_size; c++) syn1[c + l2] += g * m_neu1[c];
    }
  }
  //negative sampling
  if (m_doc2vec->negative() > 0) {
    for (d = 0; d < m_doc2vec->negative() + 1; d++) {
      if (d == 0) {
	target = central_word;
	label = 1;
      } else {
	target = negative_sample();
	if (target == central_word) continue;
	label = 0;
      }
      l2 = target * layer1_size;
      real f = 0, g = 0;      
      for (c = 0; c < layer1_size; c++) f += m_neu1[c] * syn1neg[c + l2];
      if (f > MAX_EXP) g = (label - 1) * m_doc2vec->m_alpha;
      else if (f < -MAX_EXP) g = (label - 0) * m_doc2vec->m_alpha;
      else g = (label - m_doc2vec->m_expTable[(int)((f + MAX_EXP) * (EXP_TABLE_SIZE / MAX_EXP / 2))]) * m_doc2vec->m_alpha;
      for (c = 0; c < layer1_size; c++) m_neu1e[c] += g * syn1neg[c + l2];
      if(!m_infer) for (c = 0; c < layer1_size; c++) syn1neg[c + l2] += g * m_neu1[c];
    }
  }
  if (!m_infer) {
    for (long long a = context_start; a < context_end; a++) {
      if (a != central)	{
	last_word = m_sen[a];
	for (c = 0; c < layer1_size; c++) syn0[c + last_word * layer1_size] += m_neu1e[c];
      }
    }
  }
  for (c = 0; c < layer1_size; c++) m_doc_vector[c] += m_neu1e[c];
}

void TrainModelThread::trainPairSg(long long central_word, real * context)
{
  long long c, d, l2, target, label;
  long long layer1_size = m_doc2vec->nn().dim();
  auto syn1 = m_doc2vec->nn().get_syn1();
  auto syn1neg = m_doc2vec->nn().get_syn1neg();
  for (c = 0; c < layer1_size; c++) m_neu1e[c] = 0;
  //hierarchical softmax
  if (m_doc2vec->m_hs) {
    auto & vocab = m_doc2vec->wvocab().getWords();
    for (d = 0; d < vocab[central_word].codelen; d++) {
      real f = 0;
      l2 = vocab[central_word].point[d] * layer1_size;
      for (c = 0; c < layer1_size; c++) f += context[c] * syn1[c + l2];
      if (f <= -MAX_EXP) continue;
      else if (f >= MAX_EXP) continue;
      else f = m_doc2vec->m_expTable[(int)((f + MAX_EXP) * (EXP_TABLE_SIZE / MAX_EXP / 2))];
      real g = (1 - vocab[central_word].code[d] - f) * m_doc2vec->m_alpha;
      for (c = 0; c < layer1_size; c++) m_neu1e[c] += g * syn1[c + l2];
      if(!m_infer) for (c = 0; c < layer1_size; c++) syn1[c + l2] += g * context[c];
    }
  }
  //negative sampling
  if (m_doc2vec->negative() > 0) {
    for (d = 0; d < m_doc2vec->negative() + 1; d++) {
      if (d == 0) {
	target = central_word;
	label = 1;
      } else {
	target = negative_sample();
	if (target == central_word) continue;
	label = 0;
      }
      l2 = target * layer1_size;
      real f = 0, g = 0;
      for (c = 0; c < layer1_size; c++) f += context[c] * syn1neg[c + l2];
      if (f > MAX_EXP) g = (label - 1) * m_doc2vec->m_alpha;
      else if (f < -MAX_EXP) g = (label - 0) * m_doc2vec->m_alpha;
      else g = (label - m_doc2vec->m_expTable[(int)((f + MAX_EXP) * (EXP_TABLE_SIZE / MAX_EXP / 2))]) * m_doc2vec->m_alpha;
      for (c = 0; c < layer1_size; c++) m_neu1e[c] += g * syn1neg[c + l2];
      if(!m_infer) for (c = 0; c < layer1_size; c++) syn1neg[c + l2] += g * context[c];
    }
  }
  for (c = 0; c < layer1_size; c++) context[c] += m_neu1e[c];
}

void TrainModelThread::trainSampleSg(long long central, long long context_start, long long context_end)
{  
  long long central_word = m_sen[central];
  for(long long a = context_start; a < context_end; a++) if(a != central)
  {
    long long last_word = m_sen[a];
    trainPairSg(central_word, &(m_doc2vec->nn().get_syn0()[last_word * m_doc2vec->nn().dim()]));
  }
}

void TrainModelThread::trainDocument()
{
  for(long long sentence_position = 0; sentence_position < m_sen.size(); sentence_position++)
  {
    m_next_random = m_next_random * (unsigned long long)25214903917 + 11;
    long long b = m_next_random % m_doc2vec->m_window;
    long long context_start = MAX(0LL, sentence_position - m_doc2vec->m_window + b);
    long long context_end = MIN(sentence_position + m_doc2vec->m_window - b + 1, m_sen.size());
    if(m_doc2vec->m_cbow)
    {
      trainSampleCbow(sentence_position, context_start, context_end);
    }
    else
    {
      if(!m_infer) trainSampleSg(sentence_position, context_start, context_end);
    }
  }
  if(!m_doc2vec->m_cbow)
  {
    for(size_t a = 0; a < m_sen_nosample.size(); a++)
    {
      long long last_word = m_sen_nosample[a];
      trainPairSg(last_word, m_doc_vector);
    }
  }
}

bool TrainModelThread::down_sample(long long cn)
{
  if (m_doc2vec->m_sample > 0)
  {
    real ran = (sqrt(cn / (m_doc2vec->m_sample * m_doc2vec->wvocab().getTrainWords())) + 1) *
      (m_doc2vec->m_sample * m_doc2vec->wvocab().getTrainWords()) / cn;
    m_next_random = m_next_random * (unsigned long long)25214903917 + 11;
    if (ran < (m_next_random & 0xFFFF) / (real)65536) return true;
  }
  return false;
}

long long TrainModelThread::negative_sample()
{
  m_next_random = m_next_random * (unsigned long long)25214903917 + 11;
  long long target = m_doc2vec->getNegativeSampleTable()[(m_next_random >> 16) % negative_sample_table_size];
  if (target == 0) target = m_next_random % (m_doc2vec->wvocab().size() - 1) + 1;
  return target;
}

real TrainModelThread::doc_likelihood()
{
  real likelihood = 0;
  for (size_t sentence_position = 0; sentence_position < m_sen_nosample.size(); sentence_position++) {
    likelihood += context_likelihood(sentence_position);
  }
  return likelihood;
}

real TrainModelThread::context_likelihood(long long sentence_position)
{
  real likelihood = 0;
  auto syn0 = m_doc2vec->nn().get_syn0();
  long long layer1_size = m_doc2vec->nn().dim();
  long long context_start = MAX(0LL, sentence_position - m_doc2vec->m_window);
  long long context_end = MIN(sentence_position + m_doc2vec->m_window + 1, m_sen.size());
  if (m_doc2vec->m_cbow) {
    // mean vector
    for (long long c = 0; c < layer1_size; c++) m_neu1[c] = 0;
    long long cw = 0;
    for (long long a = context_start; a < context_end; a++) {
      if (sentence_position != a) {
	long long last_word = m_sen_nosample[a];
	for (long long c = 0; c < layer1_size; c++) m_neu1[c] += syn0[c + last_word * layer1_size];
	cw++;
      }
    }
    for (long long c = 0; c < layer1_size; c++) m_neu1[c] /= cw;
    likelihood += likelihoodPair(m_sen_nosample[sentence_position], m_neu1.get());
  } else {
    for (long long a = context_start; a < context_end; a++) {
      if (sentence_position != a) {
	real * context_vector = &(syn0[layer1_size * a]);
	likelihood += likelihoodPair(m_sen_nosample[sentence_position], context_vector);
      }
    }
  }
  return likelihood;
}

real TrainModelThread::likelihoodPair(long long central, real * context_vector)
{
  long long c, d, l2, label;
  real likelihood = 0, f = 0;
  long long layer1_size = m_doc2vec->nn().dim();
  auto syn1 = m_doc2vec->nn().get_syn1();
  auto & vocab = m_doc2vec->wvocab().getWords();
  for (d = 0; d < vocab[central].codelen; d++){
    l2 = vocab[central].point[d] * layer1_size;
    label = vocab[central].code[d];
    label = label == 0 ? -1 : 1;
    for (c = 0; c < layer1_size; c++) f += context_vector[c] * syn1[c + l2];
    likelihood += -1.0 * log(1.0 + exp(label * f) );
  }
  return likelihood;
}
